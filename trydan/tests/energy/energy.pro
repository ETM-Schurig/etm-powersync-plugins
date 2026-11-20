# Offline test of sessionEnergy / totalEnergyConsumed.
# Not part of the package build. Run:  qmake6 && make && ./tst_trydanenergy
TEMPLATE = app
TARGET = tst_trydanenergy
QT += testlib
QT -= gui
CONFIG += testcase console c++17
CONFIG -= app_bundle

INCLUDEPATH += ../..

SOURCES += \
    tst_trydanenergy.cpp \
    ../../trydanenergy.cpp

HEADERS += \
    ../../trydanenergy.h
