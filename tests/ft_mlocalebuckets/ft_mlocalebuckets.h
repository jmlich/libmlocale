/****************************************************************************
**
** Copyright(C) 2010, 2011 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation(directui@nokia.com)
**
** This file is part of systemui.
**
** If you have questions regarding the use of this file, please contact
** Nokia at directui@nokia.com.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#ifndef FT_MLOCALEBUCKETS_H
#define FT_MLOCALEBUCKETS_H

#include <QObject>

namespace ML10N {
    class MLocaleBuckets;
}

using ML10N::MLocaleBuckets;

class Ft_MLocaleBuckets: public QObject
{
    Q_OBJECT

private slots:
    void cleanupTestCase();
    void initTestCase();
    void testEnglishGrouping();
    void testRemove();
    void testCopy();

#if !defined(ALSO_VERIFY_ICU_DOES_ITS_JOB_AS_WE_EXPECT)
private:
#endif
    void testChinesePinyinGrouping();
    void testGermanPhonebookGrouping();
    void testCzechGrouping();

    void sortTestFiles_data();
    void sortTestFiles();

private:
    void dumpBuckets(const MLocaleBuckets &buckets, const char *header=0) const;
    bool checkBucketContent(const MLocaleBuckets &buckets, int bucketIndex, const QStringList &expectedItems) const;
};

#endif // FT_MLOCALEBUCKETS_H

