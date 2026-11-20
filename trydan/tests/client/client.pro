# The HTTP client against a local fake Trydan (no charger needed, loopback only).
# Not part of the package build. Run:  qmake6 && make && ./tst_trydanclient
TEMPLATE = app
TARGET = tst_trydanclient
QT += testlib network
QT -= gui
CONFIG += testcase console c++17
CONFIG -= app_bundle

INCLUDEPATH += ../.. ../common

SOURCES += \
    tst_trydanclient.cpp \
    ../common/faketrydan.cpp \
    ../../trydanclient.cpp \
    ../../trydanrealtimedata.cpp

HEADERS += \
    ../common/faketrydan.h \
    ../../trydanclient.h \
    ../../trydanrealtimedata.h
