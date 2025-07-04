/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "commanddevice.h"

#include <QEventLoop>
#include <QTextStream>

#include "leakdetector.h"
#include "models/devicemodel.h"
#include "mozillavpn.h"
#include "tasks/removedevice/taskremovedevice.h"

CommandDevice::CommandDevice(QObject* parent)
    : Command(parent, "device", "Remove a device by its id.") {
  MZ_COUNT_CTOR(CommandDevice);
}

CommandDevice::~CommandDevice() { MZ_COUNT_DTOR(CommandDevice); }

int CommandDevice::run(QStringList& tokens) {
  Q_ASSERT(!tokens.isEmpty());
  return MozillaVPN::runCommandLineApp([&]() {
    if (tokens.length() != 2) {
      QTextStream stream(stdout);
      stream << "usage: " << tokens[0] << " <device_id>" << Qt::endl;
      stream << Qt::endl;
      stream << "The list of <device_id> can be obtained using: 'status'"
             << Qt::endl;
      return 1;
    }

    MozillaVPN vpn;
    if (!vpn.hasToken()) {
      QTextStream stream(stdout);
      stream << "User status: not authenticated" << Qt::endl;
      return 1;
    }
    if (!vpn.loadModels()) {
      QTextStream stream(stdout);
      stream << "No cache available" << Qt::endl;
      return 1;
    }

    bool ok;
    int id = tokens[1].toUInt(&ok);
    if (!ok) {
      QTextStream stream(stdout);
      stream << tokens[1] << " is not a valid number." << Qt::endl;
      return 1;
    }

    DeviceModel* dm = vpn.deviceModel();
    Q_ASSERT(dm);

    const QList<Device>& devices = dm->devices();
    if (id == 0 || id > devices.length()) {
      QTextStream stream(stdout);
      stream << tokens[1] << " is not a valid ID." << Qt::endl;
      return 1;
    }

    const Device& device = devices.at(id - 1);
    if (device.isCurrentDevice(vpn.keys())) {
      QTextStream stream(stdout);
      stream
          << "Removing the current device is not allowed. Use 'logout' instead."
          << Qt::endl;
      return 1;
    }

    TaskRemoveDevice task(device.publicKey());
    task.run();

    QEventLoop loop;
    QObject::connect(&task, &Task::completed, &task, [&] { loop.exit(); });
    loop.exec();

    return 0;
  });
}

static Command::RegistrationProxy<CommandDevice> s_commandDevice;
