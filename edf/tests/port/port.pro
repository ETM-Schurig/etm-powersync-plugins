# TicPort on a pseudo-terminal opened by the test (no serial hardware). Not part of the
# package build. Run:  qmake6 && make && ./tst_ticport
TEMPLATE = app
TARGET = tst_ticport
QT += testlib serialport
QT -= gui
CONFIG += testcase console c++17
CONFIG -= app_bundle
LIBS += -lutil

INCLUDEPATH += ../..

SOURCES += \
    tst_ticport.cpp \
    ../../ticdecoder.cpp \
    ../../ticport.cpp \
    ../../ticreading.cpp

HEADERS += \
    ../../ticdecoder.h \
    ../../ticport.h \
    ../../ticreading.h
