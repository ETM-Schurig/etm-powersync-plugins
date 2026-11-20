# Offline test of the RealTimeData parsing and of the state rules.
# Not part of the package build. Run:  qmake6 && make && ./tst_trydanmodel
TEMPLATE = app
TARGET = tst_trydanmodel
QT += testlib
QT -= gui
CONFIG += testcase console c++17
CONFIG -= app_bundle

INCLUDEPATH += ../..
# The metadata, to check the wiring choices the model reads.
DEFINES += TRYDAN_PLUGIN_JSON=\\\"$$PWD/../../integrationplugintrydan.json\\\"

SOURCES += \
    tst_trydanmodel.cpp \
    ../../trydanrealtimedata.cpp

HEADERS += \
    ../../trydanrealtimedata.h
