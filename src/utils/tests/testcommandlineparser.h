/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <QObject>

#include "testhelper.h"

class TestCommandLineParser : public QObject,
                              TestHelper<TestCommandLineParser> {
  Q_OBJECT

 private slots:
  void basic_data();
  void basic();
};
