# Offline test of the site coordinates rule: a site without entered coordinates is refused.
# Not part of the package build. Run:  qmake6 && make && ./tst_sitecoordinates
TEMPLATE = app
TARGET = tst_sitecoordinates
QT += testlib
QT -= gui
CONFIG += testcase console
CONFIG -= app_bundle

INCLUDEPATH += ../..
DEFINES += PLUGIN_JSON=\\\"$$PWD/../../integrationpluginopenmeteo.json\\\"

SOURCES += \
    tst_sitecoordinates.cpp \
    ../../sitecoordinates.cpp

HEADERS += \
    ../../sitecoordinates.h
