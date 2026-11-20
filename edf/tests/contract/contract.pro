# The contract, the price slots and the EJP calendar. Not part of the package build.
# Run:  qmake6 && make && ./tst_contract
TEMPLATE = app
TARGET = tst_contract
QT += testlib
QT -= gui
CONFIG += testcase console c++17
CONFIG -= app_bundle

INCLUDEPATH += ../..
RESOURCES += ../../edf.qrc

SOURCES += \
    tst_contract.cpp \
    ../../contract.cpp \
    ../../ejpcalendar.cpp \
    ../../ejpdeclaration.cpp \
    ../../priceschedule.cpp \
    ../../regulatedtariff.cpp \
    ../../ticdecoder.cpp \
    ../../ticreading.cpp

HEADERS += \
    ../../contract.h \
    ../../ejpcalendar.h \
    ../../ejpdeclaration.h \
    ../../priceschedule.h \
    ../../regulatedtariff.h \
    ../../ticdecoder.h \
    ../../ticreading.h
