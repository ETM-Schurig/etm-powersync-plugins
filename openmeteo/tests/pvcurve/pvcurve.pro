# Offline test of the forecastCurveTransport builder, against recorded Open-Meteo replies.
# Not part of the package build. Run:  qmake6 && make && ./tst_pvcurve
TEMPLATE = app
TARGET = tst_pvcurve
QT += testlib
QT -= gui
CONFIG += testcase console
CONFIG -= app_bundle

INCLUDEPATH += ../..
DEFINES += FIXTURES_DIR=\\\"$$PWD/../fixtures\\\"

SOURCES += \
    tst_pvcurve.cpp \
    ../../pvcurve.cpp

HEADERS += \
    ../../pvcurve.h
