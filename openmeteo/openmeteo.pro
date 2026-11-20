include(../plugins.pri)

QT *= network

SOURCES += \
    integrationpluginopenmeteo.cpp \
    pvcurve.cpp \
    hourlyseries.cpp \
    ensemblespread.cpp \
    retryschedule.cpp \
    sitecoordinates.cpp

HEADERS += \
    integrationpluginopenmeteo.h \
    pvcurve.h \
    hourlyseries.h \
    ensemblespread.h \
    retryschedule.h \
    sitecoordinates.h
