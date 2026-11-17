/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUENGINEUI_H
#define KISGPUENGINEUI_H

#include <QGroupBox>
#include <QString>

#include "gpu/KisGpuEngineSettings.h"
#include "kis_types.h"
#include "kritaui_export.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QWidget;
class KisDocument;
class KoColorSpace;

/**
 * User-facing parts of the GPU engine (Solstice phase 3.3,
 * docs/agent/gpu-engine.md): converting opened documents to RGBA float,
 * telling the user when the engine stopped after losing GPU content, and
 * warning before saving afterwards.
 */
class KRITAUI_EXPORT KisGpuEngineUi
{
public:
    /**
     * Registers the engine failure listener (once per process). Called by
     * every KisMainWindow; the message is shown on the GUI thread.
     */
    static void install();

    /**
     * True if a document in @p colorSpace should be offered the conversion
     * to RGBA float: the GPU engine is enabled and usable and @p colorSpace
     * is not a GPU color space.
     */
    static bool shouldOfferConversion(const KoColorSpace *colorSpace);

    /**
     * After @p document was opened: depending on
     * KisGpuEngineSettings::convertPolicy(), asks the user, converts, or
     * keeps the document. The conversion is a normal, undoable image color
     * space conversion.
     */
    static void offerConversion(KisDocument *document, QWidget *parent);

    /// Starts converting @p image to KisGpuEngineSettings::conversionTarget(). False if not needed.
    static bool convertForGpu(KisImageSP image);
    static bool convertForGpu(KisImageSP image, KisGpuEngineSettings::ConvertPolicy policy);

    /**
     * Before saving: if the GPU engine lost content in this session, warns
     * the user. Returns false to cancel saving; sets @p saveAs when the user
     * chose to save to a new file. True without asking otherwise.
     */
    static bool confirmSave(QWidget *parent, bool *saveAs);

    /**
     * KisImportExportManager, after the filter wrote @p document to its
     * temporary file and before that replaces the target: false if GPU
     * content was lost since the user last agreed to save after a loss
     * (confirmSave()), for example by a tile download that failed during
     * this very save. The export then fails and the target is not touched;
     * the next save asks first. Autosaves always pass.
     */
    static bool mayFinishExport(const KisDocument *document);

    /// Tiles whose GPU content was lost in this process (0 without the GPU engine).
    static quint64 lostTileCount();
    /// Tests: the loss count the user agreed to save with.
    static void setAcknowledgedLossCountForTesting(quint64 count);

    /// The failure message (also used by tests).
    static QString failureMessage(quint64 lostTiles);
};

/// The GPU engine group of Preferences > Performance > General.
class KRITAUI_EXPORT KisGpuEngineSettingsWidget : public QGroupBox
{
public:
    explicit KisGpuEngineSettingsWidget(QWidget *parent = nullptr);

    void load(bool requestDefault);
    void save();

private:
    QCheckBox *m_enabled;
    /// Solstice: the GPU brush (Solstice/GpuBrush)
    QCheckBox *m_brush;
    QCheckBox *m_unverifiedDevice;
    QComboBox *m_convertPolicy;
    QLabel *m_status;
};

#endif // KISGPUENGINEUI_H
