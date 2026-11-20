# Offline tests of the ensemble spread and of its freshness, against a recorded reply.
# Not part of the package build. Run:  qmake6 && make && ./tst_ensemblespread
TEMPLATE = app
TARGET = tst_ensemblespread
QT += testlib
QT -= gui
CONFIG += testcase console
CONFIG -= app_bundle

INCLUDEPATH += ../..
DEFINES += FIXTURES_DIR=\\\"$$PWD/../fixtures\\\"

SOURCES += \
    tst_ensemblespread.cpp \
    ../../ensemblespread.cpp \
    ../../pvcurve.cpp

HEADERS += \
    ../../ensemblespread.h \
    ../../pvcurve.h
