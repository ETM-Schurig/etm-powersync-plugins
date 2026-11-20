# Offline test of the retry schedule, on a simulated clock.
# Not part of the package build. Run:  qmake6 && make && ./tst_retryschedule
TEMPLATE = app
TARGET = tst_retryschedule
QT += testlib
QT -= gui
CONFIG += testcase console
CONFIG -= app_bundle

INCLUDEPATH += ../..

SOURCES += \
    tst_retryschedule.cpp \
    ../../retryschedule.cpp

HEADERS += \
    ../../retryschedule.h
