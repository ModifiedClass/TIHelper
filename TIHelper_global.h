#ifndef TIHELPER_GLOBAL_H
#define TIHELPER_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(TIHELPER_LIBRARY)
#  define TIHELPER_EXPORT Q_DECL_EXPORT
#else
#  define TIHELPER_EXPORT Q_DECL_IMPORT
#endif

#endif // TIHELPER_GLOBAL_H
