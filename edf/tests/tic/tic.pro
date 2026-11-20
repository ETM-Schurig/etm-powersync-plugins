# The historic TIC decoder (no meter, no serial port). Not part of the package build.
# Run:  qmake6 && make && ./tst_tic
TEMPLATE = app
TARGET = tst_tic
QT += testlib
QT -= gui
CONFIG += testcase console c++17
CONFIG -= app_bundle

INCLUDEPATH += ../..

SOURCES += \
    tst_tic.cpp \
    ../../ticdecoder.cpp \
    ../../ticreading.cpp

HEADERS += \
    ../../ticdecoder.h \
    ../../ticreading.h
