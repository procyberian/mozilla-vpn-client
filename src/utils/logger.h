/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LOGGER_H
#define LOGGER_H

#include <QIODevice>
#include <QObject>
#include <QString>
#include <QTextStream>

#include "loglevel.h"

#ifdef Q_OS_APPLE
#  include <CoreFoundation/CoreFoundation.h>
#endif

class QJsonObject;

class Logger {
 public:
  Logger(const QString& className);

  const QString& className() const { return m_className; }

  class Log {
   public:
    Log(Logger* logger, LogLevel level);
    ~Log();

    Log& operator<<(uint64_t t);
    Log& operator<<(const char* t);
    Log& operator<<(const QString& t);
    Log& operator<<(const QStringList& t);
    Log& operator<<(const QByteArray& t);
    Log& operator<<(const QJsonObject& t);
    Log& operator<<(QTextStreamFunction t);
    Log& operator<<(const void* t);
#ifdef Q_OS_APPLE
    Log& operator<<(const NSString* t);
    Log& operator<<(CFStringRef t);
    Log& operator<<(CFErrorRef t);
#endif

    // Q_ENUM
    template <typename T>
    typename std::enable_if<QtPrivate::IsQEnumHelper<T>::Value, Log&>::type operator<<(T t) {
      const QMetaObject* meta = qt_getEnumMetaObject(t);
      const char* name = qt_getEnumName(t);
      addMetaEnum(typename QFlags<T>::Int(t), meta, name);
      return *this;
    }

   private:
    void addMetaEnum(quint64 value, const QMetaObject* meta, const char* name);

    Logger* m_logger;
    LogLevel m_logLevel;

    struct Data {
      Data() : m_ts(&m_buffer, QIODevice::WriteOnly) {}

      QString m_buffer;
      QTextStream m_ts;
    };

    Data* m_data;
  };

  Log error();
  Log warning();
  Log info();
  Log debug();

  // Use this to log sensitive data such as IP address, session tokens, and etc.
  // When compiled with debug, this allows the sensitive data to be logged.
  QString sensitive(const QString& input);

  // Use this to log keys, which should always be obscured.
  // When compiled with debug, this truncates the keys instead for readability.
  QString keys(const QString& input);

 private:
  QString m_className;
};

#endif  // LOGGER_H
