/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "serverlatency.h"

#include <QDateTime>

#include "controller.h"
#include "feature/feature.h"
#include "leakdetector.h"
#include "logger.h"
#include "mfbt/checkedint.h"
#include "models/location.h"
#include "models/servercountrymodel.h"
#include "mozillavpn.h"
#include "pingsenderfactory.h"
#include "tcppingsender.h"

constexpr const int SERVER_LATENCY_MAX_PARALLEL = 8;

constexpr const int SERVER_LATENCY_MAX_RETRIES = 2;

// Minimum number of redundant servers we expect at a location.
constexpr int SCORE_SERVER_REDUNDANCY_THRESHOLD = 3;

// Latency threshold for excellent connections, set intentionally very low.
constexpr int SCORE_EXCELLENT_LATENCY_THRESHOLD = 30;

namespace {
Logger logger("ServerLatency");

using namespace std::chrono_literals;
constexpr const std::chrono::milliseconds SERVER_LATENCY_TIMEOUT = 5s;
constexpr const auto SERVER_LATENCY_INITIAL = 1s;
constexpr const auto SERVER_LATENCY_REFRESH = 30min;
// Delay the progressChanged() signal to rate-limit how often score changes.
constexpr const auto SERVER_LATENCY_PROGRESS_DELAY = 500ms;
}  // namespace

ServerLatency::ServerLatency() { MZ_COUNT_CTOR(ServerLatency); }

ServerLatency::~ServerLatency() { MZ_COUNT_DTOR(ServerLatency); }

void ServerLatency::initialize() {
  MozillaVPN* vpn = MozillaVPN::instance();

  connect(vpn->serverCountryModel(), &ServerCountryModel::changed, this,
          &ServerLatency::start);

  connect(vpn->controller(), &Controller::stateChanged, this,
          &ServerLatency::stateChanged);

  connect(&m_pingTimeout, &QTimer::timeout, this,
          &ServerLatency::maybeSendPings);

  m_refreshTimer.setSingleShot(true);
  connect(&m_refreshTimer, &QTimer::timeout, this, &ServerLatency::start);

  m_progressDelayTimer.setSingleShot(true);
  connect(&m_progressDelayTimer, &QTimer::timeout, this,
          [this]() { emit progressChanged(); });

  const Feature* feature = Feature::get(Feature::Feature_serverConnectionScore);
  connect(feature, &Feature::supportedChanged, this, &ServerLatency::start);
  if (feature->isSupported()) {
    m_refreshTimer.start(SERVER_LATENCY_INITIAL);
  }

  m_cooldownTimer.setSingleShot(true);
  connect(&m_cooldownTimer, &QTimer::timeout, this,
          &ServerLatency::clearCooldowns);

  connect(qApp, &QApplication::applicationStateChanged, this,
          &ServerLatency::applicationStateChanged);
}

void ServerLatency::start() {
  MozillaVPN* vpn = MozillaVPN::instance();
  if (!Feature::get(Feature::Feature_serverConnectionScore)->isSupported()) {
    clear();
    return;
  }

  if (vpn->controller()->state() != Controller::StateOff) {
    // Don't attempt to refresh latency when the VPN is active, or
    // we could get misleading results.
    m_wantRefresh = true;
    return;
  }
  if (m_pingSender != nullptr) {
    // Don't start a latency refresh if one is already in progress.
    return;
  }

  m_sequence = 0;
  m_wantRefresh = false;
  m_pingSender = PingSenderFactory::create(QHostAddress(), this);
  if (!m_pingSender->isValid()) {
    // Fallback to using TCP handshake times for pings if we can't create an
    // ICMP socket on this platform, this probes at the ports used for Wireguard
    // over TCP.
    delete m_pingSender;
    m_pingSender = new TcpPingSender(QHostAddress(), 80, this);
  }

  connect(m_pingSender, SIGNAL(recvPing(quint16)), this,
          SLOT(recvPing(quint16)), Qt::QueuedConnection);
  connect(m_pingSender, SIGNAL(criticalPingError()), this,
          SLOT(criticalPingError()));

  // Generate a list of servers to ping. If possible, sort them by geographic
  // distance to try and get data for the quickest servers first.
  for (const ServerCountry& country : vpn->serverCountryModel()->countries()) {
    for (const QString& cityName : country.cities()) {
      const ServerCity& city =
          vpn->serverCountryModel()->findCity(country.code(), cityName);
      double distance =
          vpn->location()->distance(city.latitude(), city.longitude());
      Q_ASSERT(city.initialized());

      // Search for where in the list to insert this city's servers.
      auto i = m_pingSendQueue.begin();
      while (i != m_pingSendQueue.end()) {
        if (i->distance >= distance) {
          break;
        }
        i++;
      }

      // Insert the servers into the list.
      for (const QString& pubkey : city.servers()) {
        ServerPingRecord rec = {
            pubkey, city.country(), city.name(), 0, 0, distance, 0};
        i = m_pingSendQueue.insert(i, rec);
      }
    }
  }

  m_pingSendTotal = m_pingSendQueue.count();

  m_progressDelayTimer.stop();
  emit progressChanged();

  m_refreshTimer.stop();
  maybeSendPings();
}

void ServerLatency::maybeSendPings() {
  quint64 now = QDateTime::currentMSecsSinceEpoch();
  ServerCountryModel* scm = MozillaVPN::instance()->serverCountryModel();
  if (m_pingSender == nullptr) {
    return;
  }

  // Scan through the reply list, looking for timeouts.
  while (!m_pingReplyList.isEmpty()) {
    const ServerPingRecord& record = m_pingReplyList.first();
    if ((record.timestamp + SERVER_LATENCY_TIMEOUT.count()) > now) {
      break;
    }
    logger.debug() << "Server" << logger.keys(record.publicKey) << "timeout"
                   << record.retries;

    // Send a retry.
    if (record.retries < SERVER_LATENCY_MAX_RETRIES) {
      ServerPingRecord retry = record;
      retry.retries++;
      retry.sequence = m_sequence++;
      retry.timestamp = now;
      m_pingReplyList.append(retry);

      const Server& server = scm->server(retry.publicKey);
      m_pingSender->sendPing(QHostAddress(server.ipv4AddrIn()), retry.sequence);
    }

    // TODO: Mark the server unavailable?
    m_pingReplyList.removeFirst();
  }

  // Generate new pings until we reach our max number of parallel pings.
  while (m_pingReplyList.count() < SERVER_LATENCY_MAX_PARALLEL) {
    if (m_pingSendQueue.isEmpty()) {
      break;
    }

    ServerPingRecord record = m_pingSendQueue.takeFirst();
    record.sequence = m_sequence++;
    record.timestamp = now;
    record.retries = 0;
    m_pingReplyList.append(record);

    const Server& server = scm->server(record.publicKey);
    m_pingSender->sendPing(QHostAddress(server.ipv4AddrIn()), record.sequence);
  }

  m_lastUpdateTime = QDateTime::currentDateTime();
  if (!m_progressDelayTimer.isActive()) {
    m_progressDelayTimer.start(SERVER_LATENCY_PROGRESS_DELAY);
  }

  if (m_pingReplyList.isEmpty()) {
    // If the ping reply list is empty, then we have nothing left to do.
    stop();
  } else {
    // Otherwise, the list should be sorted by transmit time. Schedule a timer
    // to cleanup anything that experiences a timeout.
    const ServerPingRecord& record = m_pingReplyList.first();

    CheckedInt<int> value(SERVER_LATENCY_TIMEOUT.count());
    value -= static_cast<int>(now - record.timestamp);

    m_pingTimeout.start(value.value());
  }
}

void ServerLatency::stop() {
  m_pingTimeout.stop();
  m_pingSendQueue.clear();
  m_pingReplyList.clear();
  m_pingSendTotal = 0;

  if (m_pingSender) {
    m_pingSender->deleteLater();
    m_pingSender = nullptr;
  }

  emit progressChanged();
  m_progressDelayTimer.stop();
  if (!m_refreshTimer.isActive()) {
    m_refreshTimer.start(SERVER_LATENCY_REFRESH);
  }
}

void ServerLatency::refresh() {
  if (m_pingSender) {
    return;
  }

  clear();
  start();
}

void ServerLatency::clear() {
  m_latency.clear();
  m_sumLatencyMsec = 0;

  emit progressChanged();
}

void ServerLatency::stateChanged() {
  Controller::State state = MozillaVPN::instance()->controller()->state();
  if (state != Controller::StateOff) {
    // If the VPN is active, then do not attempt to measure the server latency.
    stop();
  } else if (m_wantRefresh) {
    // If the VPN has been deactivated, start a refresh if desired.
    start();
  }
}

// iOS kills the socket shortly after the device is turned off, and possibly if
// the app is backgrounded. This was causing a crash when the device was turned
// back on. By only refreshing the server list when the app is active, we
// prevent this crash. More details in VPN-5766.
void ServerLatency::applicationStateChanged() {
#ifdef MZ_IOS
  if (QGuiApplication::applicationState() !=
      Qt::ApplicationState::ApplicationActive) {
    if (m_pingSender != nullptr) {
      m_wantRefresh = true;
      stop();
    }
  } else {
    if (m_wantRefresh) {
      refresh();
    }
  }
#endif
}

void ServerLatency::recvPing(quint16 sequence) {
  qint64 now(QDateTime::currentMSecsSinceEpoch());

  for (auto i = m_pingReplyList.begin(); i != m_pingReplyList.end(); i++) {
    const ServerPingRecord& record = *i;
    if (record.sequence != sequence) {
      continue;
    }

    qint64 latency(now - record.timestamp);
    if (latency <= std::numeric_limits<uint>::max()) {
      setLatency(record.publicKey, latency);
    }

    m_pingReplyList.erase(i);
    maybeSendPings();
    return;
  }
}

void ServerLatency::criticalPingError() {
  logger.info() << "Encountered Unrecoverable ping error";
}

qint64 ServerLatency::avgLatency() const {
  if (m_latency.isEmpty()) {
    return 0;
  }
  return (m_sumLatencyMsec + m_latency.count() - 1) / m_latency.count();
}

void ServerLatency::setLatency(const QString& pubkey, qint64 msec) {
  m_sumLatencyMsec -= m_latency[pubkey];
  m_sumLatencyMsec += msec;
  m_latency[pubkey] = msec;

  updateConnectionScore(pubkey);
}

void ServerLatency::updateConnectionScore(const QString& pubkey) {
  ServerCountryModel* scm = MozillaVPN::instance()->serverCountryModel();
  const Server& server = scm->server(pubkey);
  ServerCity& city = scm->findCity(server.countryCode(), server.cityName());

  // Update the average latency for this city.
  int numLatencySamples = 0;
  qint64 avgLatencyMsec = 0;
  for (const QString& pubkey : city.servers()) {
    qint64 rtt = m_latency.value(pubkey);
    if (rtt > 0) {
      avgLatencyMsec += rtt;
      numLatencySamples++;
    }
  }
  if (numLatencySamples > 0) {
    avgLatencyMsec += (numLatencySamples - 1);
    avgLatencyMsec /= numLatencySamples;
  }
  city.setLatency(avgLatencyMsec);

  // Calculate the base score based on the user's current location.
  QString userCountry = MozillaVPN::instance()->location()->countryCode();
  int score = baseCityScore(&city, userCountry);
  if ((score <= ServerLatency::Unavailable) || (avgLatencyMsec == 0)) {
    city.setConnectionScore(score);
    return;
  }

  // Increase the score if the location has better than average latency.
  if (avgLatencyMsec < avgLatency()) {
    score++;
    // Give the location another point if the latency is *very* fast.
    if (avgLatencyMsec < SCORE_EXCELLENT_LATENCY_THRESHOLD) {
      score++;
    }
  }

  if (score > ServerLatency::Excellent) {
    score = ServerLatency::Excellent;
  }
  city.setConnectionScore(score);
}

double ServerLatency::progress() const {
  if ((m_pingSender == nullptr) || (m_pingSendTotal == 0)) {
    return 1.0;  // Operation is complete.
  }

  double remaining = m_pingReplyList.count() + m_pingSendQueue.count();
  return 1.0 - (remaining / m_pingSendTotal);
}

void ServerLatency::setCooldown(const QString& publicKey, qint64 timeout) {
  if (timeout <= 0) {
    m_cooldown.remove(publicKey);
  } else {
    m_cooldown[publicKey] = QDateTime::currentSecsSinceEpoch() + timeout;
  }

  // Update the connection score.
  updateConnectionScore(publicKey);

  // (Re)schedule the cooldown timer if this would be the next expiration.
  int next = m_cooldownTimer.remainingTime();
  if ((next <= 0) || (timeout < next)) {
    m_cooldownTimer.start(timeout);
  }
}

void ServerLatency::setCityCooldown(const QString& countryCode,
                                    const QString& cityCode, qint64 timeout) {
  ServerCountryModel* scm = MozillaVPN::instance()->serverCountryModel();
  logger.debug() << "Set cooldown for all servers for:"
                 << logger.sensitive(countryCode) << logger.sensitive(cityCode);

  // Enumerate all the servers in this city and set their cooldown.
  qint64 expire = QDateTime::currentSecsSinceEpoch() + timeout;
  QString cityName;
  for (auto city : scm->cities()) {
    if (city.country() != countryCode) {
      continue;
    }
    if (city.code() != cityCode) {
      continue;
    }
    cityName = city.name();
    for (const QString& pubkey : city.servers()) {
      m_cooldown[pubkey] = expire;
    }
  }
  if (cityName.isEmpty()) {
    logger.debug() << "no such city";
    return;
  }

  // With all servers on cooldown, the city score should be unavailable.
  ServerCity& city = scm->findCity(countryCode, cityName);
  city.setConnectionScore(Unavailable);

  // (Re)schedule the cooldown timer if this would be the next expiration.
  int next = m_cooldownTimer.remainingTime();
  if ((next <= 0) || (timeout < next)) {
    m_cooldownTimer.start(timeout);
  }
}

void ServerLatency::clearCooldowns() {
  qint64 now = QDateTime::currentSecsSinceEpoch();
  qint64 next = 0;
  for (const QString& pubkey : m_cooldown.keys()) {
    qint64 expiration = m_cooldown.value(pubkey);
    if (expiration > now) {
      if ((next == 0) || (expiration < next)) {
        next = expiration;
      }
      continue;
    }

    m_cooldown.remove(pubkey);
    updateConnectionScore(pubkey);
  }

  // (Re)schedule the cooldown timer if there are more cooldowns to expire.
  if (next > now) {
    m_cooldownTimer.start(next - now);
  }
}

int ServerLatency::baseCityScore(const ServerCity* city,
                                 const QString& originCountry) const {
  qint64 now = QDateTime::currentSecsSinceEpoch();
  int score = Poor;
  int activeServerCount = 0;
  for (const QString& pubkey : city->servers()) {
    if (getCooldown(pubkey) <= now) {
      activeServerCount++;
    }
  }

  // Ensure there is at least one reachable server.
  if (activeServerCount == 0) {
    return Unavailable;
  }

  // Increase the score if the location has sufficient redundancy.
  if (activeServerCount >= SCORE_SERVER_REDUNDANCY_THRESHOLD) {
    score++;
  }

  // Increase the score for connections made within the same country.
  if ((!originCountry.isEmpty()) &&
      (originCountry.compare(city->country(), Qt::CaseInsensitive) == 0)) {
    score++;
  }

  if (score > Excellent) {
    score = Excellent;
  }
  return score;
}

int ServerLatency::multiHopScore(const QString& exitCountry,
                                 const QString& exitCityName,
                                 const QString& entryCountry,
                                 const QString& entryCityName) const {
  const ServerCountryModel* scm = MozillaVPN::instance()->serverCountryModel();
  const ServerCity& exitCity = scm->findCity(exitCountry, exitCityName);
  logger.debug() << "multihop score" << exitCountry << exitCityName << "to"
                 << entryCountry << entryCityName;
  if (!exitCity.initialized()) {
    logger.debug() << "multihop no exit data";
    return ServerLatency::NoData;
  }

  int score = baseCityScore(&exitCity, entryCountry);
  if (score <= ServerLatency::Unavailable) {
    logger.debug() << "multihop unavailable";
    return score;
  }

  // Increase the score if the distance between servers is less than 1/8th of
  // the earth's circumference.
  const ServerCity& entryCity = scm->findCity(entryCountry, entryCityName);
  if (!entryCity.initialized()) {
    logger.debug() << "mutlihop no entry data";
    return ServerLatency::NoData;
  }
  if ((Location::distance(&exitCity, &entryCity) < (M_PI / 4))) {
    score++;
  }

  if (score > ServerLatency::Excellent) {
    score = ServerLatency::Excellent;
  }
  return score;
}
