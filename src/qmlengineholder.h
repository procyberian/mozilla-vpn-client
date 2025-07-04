/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef QMLENGINEHOLDER_H
#define QMLENGINEHOLDER_H

class QQmlEngine;
class QWindow;

class QmlEngineHolder {
 public:
  explicit QmlEngineHolder(QQmlEngine* engine);

  QmlEngineHolder(const QmlEngineHolder&) = delete;
  QmlEngineHolder& operator=(const QmlEngineHolder&) = delete;
  QmlEngineHolder(QmlEngineHolder&&) = delete;
  QmlEngineHolder& operator=(QmlEngineHolder&&) = delete;

  ~QmlEngineHolder();

  static QmlEngineHolder* instance();

  static bool exists();

  QQmlEngine* engine() { return m_engine; }

#ifdef UNIT_TEST
  void replaceEngine(QQmlEngine* engine) { m_engine = engine; }
#endif

  QWindow* window() const;
  void showWindow();
  void hideWindow();
  bool hasWindow() const;

 private:
  QQmlEngine* m_engine = nullptr;
};

#endif  // QMLENGINEHOLDER_H
