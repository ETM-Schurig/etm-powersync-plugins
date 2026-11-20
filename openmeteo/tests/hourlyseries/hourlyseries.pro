# Offline tests of the hourly series states, against recorded Open-Meteo replies.
# Not part of the package build. Run:  qmake6 && make && ./tst_hourlyseries
TEMPLATE = app
TARGET = tst_hourlyseries
QT += testlib
QT -= gui
CONFIG += testcase console
CONFIG -= app_bundle

INCLUDEPATH += ../..
DEFINES += FIXTURES_DIR=\\\"$$PWD/../fixtures\\\"

SOURCES += \
    tst_hourlyseries.cpp \
    ../../hourlyseries.cpp \
    ../../pvcurve.cpp

HEADERS += \
    ../../hourlyseries.h \
    ../../pvcurve.h
