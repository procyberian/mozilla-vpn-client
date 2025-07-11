/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "testsignupandin.h"

#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

#include "authenticationinapp/authenticationinapp.h"
#include "constants.h"
#include "networkrequest.h"
#include "taskfunction.h"
#include "tasks/authenticate/taskauthenticate.h"

constexpr const char* PASSWORD = "12345678";

class EventLoop final : public QEventLoop {
 public:
  void exec() {
    QTimer timer;
    connect(&timer, &QTimer::timeout, &timer, [&]() {
      qDebug() << "TIMEOUT!";
      exit();
    });
    timer.setSingleShot(true);
    timer.start(60000 /* 60 seconds */);

    QEventLoop::exec();
  }
};

TestSignUpAndIn::TestSignUpAndIn(const QString& nonce, const QString& pattern,
                                 bool totpCreation)
    : m_totpCreation(totpCreation) {
  QString emailAccount(pattern);
  emailAccount.append(nonce);
  m_emailAccount = emailAccount;
  qDebug() << "Pattern:" << m_emailAccount;
}

void TestSignUpAndIn::signUp() {
  AuthenticationInApp* aia = AuthenticationInApp::instance();
  QVERIFY(!!aia);
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateInitializing);

  // Starting the authentication flow.
  TaskAuthenticate task(AuthenticationListener::AuthenticationInApp);
  task.run();

  EventLoop loop;
  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    if (aia->state() == AuthenticationInApp::StateStart) {
      loop.exit();
    }
  });
  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateStart);

  QString emailAddress(m_emailAccount);
  emailAddress.append("@restmail.net");

  // Account
  aia->checkAccount(emailAddress);
  QCOMPARE(aia->state(), AuthenticationInApp::StateCheckingAccount);

  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    QVERIFY(aia->state() != AuthenticationInApp::StateSignIn);
    if (aia->state() == AuthenticationInApp::StateSignUp) {
      loop.exit();
    }
  });
  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateSignUp);

  // Password
  aia->setPassword(PASSWORD);

  if (m_totpCreation) {
    aia->enableTotpCreation();
  }

  // Sign-up
  aia->signUp();
  QCOMPARE(aia->state(), AuthenticationInApp::StateSigningUp);

  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    if (aia->state() ==
        AuthenticationInApp::StateVerificationSessionByEmailNeeded) {
      loop.exit();
    }
  });
  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(),
           AuthenticationInApp::StateVerificationSessionByEmailNeeded);

  // Email verification - with invalid code
  aia->verifySessionEmailCode("000000");
  QCOMPARE(aia->state(), AuthenticationInApp::StateVerifyingSessionEmailCode);

  connect(aia, &AuthenticationInApp::errorOccurred, aia,
          [&](AuthenticationInApp::ErrorType error, uint32_t) {
            if (error ==
                AuthenticationInApp::ErrorInvalidOrExpiredVerificationCode) {
              loop.exit();
            }
          });
  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(),
           AuthenticationInApp::StateVerificationSessionByEmailNeeded);

  // Email verification - with valid code
  QString code = fetchSessionCode();
  QVERIFY(!code.isEmpty());
  aia->verifySessionEmailCode(code);
  QCOMPARE(aia->state(), AuthenticationInApp::StateVerifyingSessionEmailCode);

  connect(&task, &Task::completed, &task, [&]() { loop.exit(); });

  if (m_totpCreation) {
    waitForTotpCodes();
  }

  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);
}

void TestSignUpAndIn::signUpWithError() {
  // This test works only for non-blocked accounts.
  if (!m_emailAccount.startsWith("vpn")) {
    return;
  }

  AuthenticationInApp* aia = AuthenticationInApp::instance();
  QVERIFY(!!aia);
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateInitializing);

  // Starting the authentication flow.
  TaskAuthenticate task(AuthenticationListener::AuthenticationInApp);
  task.run();

  EventLoop loop;
  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    if (aia->state() == AuthenticationInApp::StateStart) {
      loop.exit();
    }
  });
  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateStart);

  QString emailAddress(m_emailAccount);
  emailAddress.append("@restmail.net");

  // Account
  aia->checkAccount(emailAddress);
  QCOMPARE(aia->state(), AuthenticationInApp::StateCheckingAccount);

  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    if (aia->state() == AuthenticationInApp::StateSignIn) {
      loop.exit();
    }
  });
  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateSignIn);

  // Password
  aia->setPassword(PASSWORD);

  // Even if we are in SignIn, let's call the Sign-up
  aia->signUp();
  QCOMPARE(aia->state(), AuthenticationInApp::StateSigningUp);

  connect(
      aia, &AuthenticationInApp::errorOccurred, aia,
      [&](AuthenticationInApp::ErrorType error, uint32_t) {
        if (error == AuthenticationInApp::ErrorAccountAlreadyExists) {
          qDebug() << "The account already exist. Error correctly propagated.";
          loop.exit();
        }
      });
  loop.exec();

  disconnect(aia, nullptr, nullptr, nullptr);
}

void TestSignUpAndIn::signIn() {
  AuthenticationInApp* aia = AuthenticationInApp::instance();
  QVERIFY(!!aia);
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateInitializing);

  // Starting the authentication flow.
  TaskAuthenticate task(AuthenticationListener::AuthenticationInApp);
  task.run();

  EventLoop loop;
  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    if (aia->state() == AuthenticationInApp::StateStart) {
      loop.exit();
    }
  });
  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateStart);

  QString emailAddress(m_emailAccount);
  emailAddress.append("@restmail.net");

  // Just to make things more complex, let's pass an upper-case email address.
  aia->allowUpperCaseEmailAddress();
  emailAddress[0] = emailAddress[0].toUpper();

  // Account
  aia->checkAccount(emailAddress);
  QCOMPARE(aia->state(), AuthenticationInApp::StateCheckingAccount);

  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    if (aia->state() == AuthenticationInApp::StateSignIn) {
      loop.exit();
    }
  });

  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateSignIn);

  // Invalid Password
  if (m_emailAccount.startsWith("vpn")) {
    // We run this part only for non-blocked accounts because for them, the
    // password doesn't really matter.
    aia->setPassword("Invalid!");
    connect(aia, &AuthenticationInApp::errorOccurred, aia,
            [&](AuthenticationInApp::ErrorType error, uint32_t) {
              if (error == AuthenticationInApp::ErrorIncorrectPassword) {
                qDebug() << "Incorrect password!";
                loop.exit();
              }
            });

    // Sign-in
    aia->signIn();
    loop.exec();
  }

  aia->setPassword(PASSWORD);

  // Sign-in
  aia->signIn();
  QCOMPARE(aia->state(), AuthenticationInApp::StateSigningIn);

  // The next step can be tricky: totp, or unblocked code, or success
  if (m_totpCreation) {
    waitForTotpCodes();
  }

  bool wrongUnblockCodeSent = false;

  connect(aia, &AuthenticationInApp::errorOccurred, aia,
          [this](AuthenticationInApp::ErrorType error, uint32_t) {
            if (error == AuthenticationInApp::ErrorInvalidUnblockCode) {
              qDebug() << "Invalid unblock code. Sending a good one";
              fetchAndSendUnblockCode();
            }
          });

  connect(aia, &AuthenticationInApp::stateChanged, [&]() {
    if (!wrongUnblockCodeSent &&
        aia->state() == AuthenticationInApp::StateUnblockCodeNeeded) {
      aia->verifyUnblockCode("000000");
      QCOMPARE(aia->state(), AuthenticationInApp::StateVerifyingUnblockCode);

      wrongUnblockCodeSent = true;
    }
  });

  connect(&task, &Task::completed, &task, [&]() { loop.exit(); });

  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);
}

void TestSignUpAndIn::signInWithError() {
  AuthenticationInApp* aia = AuthenticationInApp::instance();
  QVERIFY(!!aia);
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateInitializing);

  // Starting the authentication flow.
  TaskAuthenticate task(AuthenticationListener::AuthenticationInApp);
  task.run();

  EventLoop loop;
  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    if (aia->state() == AuthenticationInApp::StateStart) {
      loop.exit();
    }
  });
  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateStart);

  QString emailAddress(m_emailAccount);
  emailAddress.append("@restmail.net");

  // Account
  aia->checkAccount(emailAddress);
  QCOMPARE(aia->state(), AuthenticationInApp::StateCheckingAccount);

  connect(aia, &AuthenticationInApp::stateChanged, aia, [&]() {
    if (aia->state() == AuthenticationInApp::StateSignUp) {
      loop.exit();
    }
  });

  loop.exec();
  disconnect(aia, nullptr, nullptr, nullptr);

  QCOMPARE(aia->state(), AuthenticationInApp::StateSignUp);

  aia->setPassword(PASSWORD);

  // Sign-in even if the account does not exist.
  aia->signIn();
  QCOMPARE(aia->state(), AuthenticationInApp::StateSigningIn);

  connect(aia, &AuthenticationInApp::errorOccurred, aia,
          [&](AuthenticationInApp::ErrorType error, uint32_t) {
            if (error == AuthenticationInApp::ErrorUnknownAccount) {
              qDebug() << "The account does not exist yet";
              loop.exit();
            }
          });
  loop.exec();
}

QString TestSignUpAndIn::fetchSessionCode() {
  return fetchCode("x-verify-short-code");
}

QString TestSignUpAndIn::fetchUnblockCode() {
  return fetchCode("x-unblock-code");
}

void TestSignUpAndIn::waitForTotpCodes() {
  qDebug() << "Adding the callback for the totp code creation";

  AuthenticationInApp* aia = AuthenticationInApp::instance();

  connect(aia, &AuthenticationInApp::errorOccurred, aia,
          [this](AuthenticationInApp::ErrorType error, uint32_t) {
            if (error == AuthenticationInApp::ErrorInvalidTotpCode) {
              qDebug() << "Invalid code. Current state:" << m_sendTotpCodeState;
              sendNextTotpCode();
            }
          });

  connect(aia, &AuthenticationInApp::unitTestTotpCodeCreated, aia,
          [this](const QByteArray& data) {
            qDebug() << "Codes received";
            QJsonDocument json = QJsonDocument::fromJson(data);
            QJsonObject obj = json.object();
            m_totpSecret = obj["secret"].toString();
            QVERIFY(!m_totpSecret.isEmpty());
          });

  connect(aia, &AuthenticationInApp::stateChanged, aia, [this]() {
    AuthenticationInApp* aia = AuthenticationInApp::instance();
    qDebug() << "Send wrong code:" << m_sendTotpCodeState;

    if (m_sendTotpCodeState == NoCodeSent &&
        aia->state() ==
            AuthenticationInApp::StateVerificationSessionByTotpNeeded) {
      sendNextTotpCode();
    }
  });
}

void TestSignUpAndIn::sendNextTotpCode() {
  AuthenticationInApp* aia = AuthenticationInApp::instance();
  QCOMPARE(aia->state(),
           AuthenticationInApp::StateVerificationSessionByTotpNeeded);

  Q_ASSERT(m_sendTotpCodeState < GoodTotpCode);
  m_sendTotpCodeState = static_cast<SendTotpCodeState>(m_sendTotpCodeState + 1);
  switch (m_sendTotpCodeState) {
    case SendWrongTotpCodeNumber:
      qDebug() << "Code required. Let's write a wrong code first (numeric).";
      aia->verifySessionTotpCode("123456");
      break;

    case SendWrongTotpCodeString:
      qDebug() << "Code required. Let's write a wrong code first (string).";
      aia->verifySessionTotpCode("aabbcc");
      break;

    case SendWrongTotpCodeAlphaNumeric:
      qDebug()
          << "Code required. Let's write a wrong code first (alphanmeric).";
      aia->verifySessionTotpCode("12345a");
      break;

    case GoodTotpCode: {
      QString otp;
      {
        QProcess process;
        process.start("python3", QStringList{"-m", "oathtool", m_totpSecret});
        QVERIFY(process.waitForStarted());

        process.closeWriteChannel();
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);

        otp = process.readAll().trimmed();
      }

      qDebug() << "Code required. Let's write a good code:" << otp;
      aia->verifySessionTotpCode(otp);
    }

      // Let's reset the state for the next cycle.
      m_sendTotpCodeState = NoCodeSent;
      break;

    default:
      Q_ASSERT(false);
      break;
  }

  QCOMPARE(aia->state(), AuthenticationInApp::StateVerifyingSessionTotpCode);
}

QString TestSignUpAndIn::fetchCode(const QString& code) {
  while (true) {
    QString url = "http://restmail.net/mail/";
    url.append(m_emailAccount);

    // In theory, network requests should be executed by tasks, but for this
    // test we do an hack.
    TaskFunction dummyTask([] {});

    NetworkRequest* nr = new NetworkRequest(&dummyTask);
    nr->get(url);

    QByteArray jsonData;
    EventLoop loop;
    connect(nr, &NetworkRequest::requestFailed, nr,
            [&](QNetworkReply::NetworkError, const QByteArray&) {
              qDebug() << "Failed to fetch the restmail.net content";
              loop.exit();
            });
    connect(nr, &NetworkRequest::requestCompleted, nr,
            [&](const QByteArray& data) {
              jsonData = data;
              loop.exit();
            });
    loop.exec();

    QJsonDocument doc(QJsonDocument::fromJson(jsonData));
    QJsonArray array = doc.array();
    if (!array.isEmpty()) {
      QJsonObject obj = array.last().toObject();
      QJsonObject headers = obj["headers"].toObject();
      if (headers.contains(code)) {
        return headers[code].toString();
      }
    }

    qDebug() << "Email not received yet";

    QTimer timer;
    connect(&timer, &QTimer::timeout, &timer, [&]() { loop.exit(); });
    timer.setSingleShot(true);
    timer.start(2000 /* 2 seconds */);
    loop.exec();
  }
}

void TestSignUpAndIn::fetchAndSendUnblockCode() {
  AuthenticationInApp* aia = AuthenticationInApp::instance();
  QCOMPARE(aia->state(), AuthenticationInApp::StateUnblockCodeNeeded);

  // We do not receive the email each time.
  aia->resendUnblockCodeEmail();

  QString code = fetchUnblockCode();
  QVERIFY(!code.isEmpty());
  aia->verifyUnblockCode(code);
  QCOMPARE(aia->state(), AuthenticationInApp::StateVerifyingUnblockCode);
}
