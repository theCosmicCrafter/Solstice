/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef _KIS_SVG_EXPORT_H_
#define _KIS_SVG_EXPORT_H_

#include <QVariant>

#include <KisImportExportFilter.h>

class KisSVGExport : public KisImportExportFilter
{
    Q_OBJECT
public:
    KisSVGExport(QObject *parent, const QVariantList &);
    ~KisSVGExport() override;

public:
    KisImportExportErrorCode
    convert(KisDocument *document, QIODevice *io, KisPropertiesConfigurationSP configuration) override;
    void initializeCapabilities() override;
};

#endif
