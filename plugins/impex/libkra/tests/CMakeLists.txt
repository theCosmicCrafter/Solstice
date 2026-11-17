kis_add_tests(
    kis_kra_loader_test.cpp
    LINK_LIBRARIES kritaui kritalibkra kritatestsdk
    )


kis_add_test(
    kis_kra_saver_test.cpp
    LINK_LIBRARIES kritaui kritalibkra kritatransformmaskstubs
    )

# Clipping Mask (Solstice, docs/agent/clipping-mask-plan.md)
kis_add_test(
    KisClippingKraTest.cpp
    LINK_LIBRARIES kritaui kritalibkra kritatestsdk
    )

# GPU engine (Solstice, docs/agent/gpu-engine.md)
kis_add_test(
    KisGpuSaveTest.cpp
    LINK_LIBRARIES kritaui kritalibkra kritatestsdk
    )

# Move Tool update costs on a real document, opt-in (Solstice,
# docs/agent/gpu-engine.md)
kis_add_test(
    KisMoveDocumentBenchmark.cpp
    LINK_LIBRARIES kritaui kritalibkra kritatestsdk
    )
