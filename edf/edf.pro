include(../plugins.pri)

# No network: the plugin opens no connection (QtNetwork not linked).
QT *= serialport

RESOURCES += edf.qrc

SOURCES += \
    integrationpluginedf.cpp \
    contract.cpp \
    ejpcalendar.cpp \
    ejpdeclaration.cpp \
    priceschedule.cpp \
    regulatedtariff.cpp \
    ticdecoder.cpp \
    ticport.cpp \
    ticreading.cpp

HEADERS += \
    integrationpluginedf.h \
    contract.h \
    ejpcalendar.h \
    ejpdeclaration.h \
    priceschedule.h \
    regulatedtariff.h \
    ticdecoder.h \
    ticport.h \
    ticreading.h
