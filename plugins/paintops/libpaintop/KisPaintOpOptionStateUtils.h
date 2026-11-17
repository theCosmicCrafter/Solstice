/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISPAINTOPOPTIONSTATEUTILS_H
#define KISPAINTOPOPTIONSTATEUTILS_H

#include <optional>
#include <tuple>

#include <KisCurveOptionModel.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <KisPaintOpOptionsModel.h>

/**
 * Helpers for option widgets whose state lives in KisPaintOpOptionsModel
 * instead of the widget. See docs/agent/brush-option-shared-model.md.
 */
namespace KisPaintOpOptionStateUtils
{

namespace detail
{
template<typename Widget, typename Data>
auto widgetCursor(KisPaintOpOptionState<Data> *state)
{
    if constexpr (KisPaintOpOptionWidgetUtils::detail::has_type_data_type<Widget>::value) {
        if constexpr (!std::is_same_v<Data, typename Widget::data_type>) {
            return state->cursor().zoom(kislager::lenses::to_base<typename Widget::data_type>);
        } else {
            return state->cursor();
        }
    } else {
        return state->cursor();
    }
}

template<typename Widget, typename Data>
struct StateWidgetWithLodLimitations : public Widget {
    template<typename... Args>
    StateWidgetWithLodLimitations(KisPaintOpOptionState<Data> *state, Args... args)
        : Widget(widgetCursor<Widget>(state), std::forward<Args>(args)...)
        , m_lodData(state->reader())
    {
    }

    KisPaintOpOption::OptionalLodLimitationsReader lodLimitationsReader() const override
    {
        return kislager::fold_optional_cursors(
            std::bit_or{},
            Widget::lodLimitationsReader(),
            KisPaintOpOption::OptionalLodLimitationsReader(m_lodData.map(std::mem_fn(&Data::lodLimitations))));
    }

private:
    lager::reader<Data> m_lodData;
};
} // namespace detail

/**
 * Creates an option widget bound to a model-owned \p state. Extra arguments
 * are forwarded after the cursor, as in
 * KisPaintOpOptionWidgetUtils::createOptionWidget().
 */
template<typename Widget, typename Data, typename... Args>
Widget *createOptionWidget(KisPaintOpOptionState<Data> *state, Args... args)
{
    return new Widget(detail::widgetCursor<Widget>(state), std::forward<Args>(args)...);
}

/**
 * Same as createOptionWidget(), but the widget also reports
 * Data::lodLimitations(), like
 * KisPaintOpOptionWidgetUtils::createOptionWidgetWithLodLimitations().
 */
template<typename Widget, typename Data, typename... Args>
Widget *createOptionWidgetWithLodLimitations(KisPaintOpOptionState<Data> *state, Args... args)
{
    return new detail::StateWidgetWithLodLimitations<Widget, Data>(state, std::forward<Args>(args)...);
}

/**
 * Cursor for the KisCurveOptionWidget-based helpers, e.g.
 * KisPaintOpOptionWidgetUtils::createOpacityOptionWidget(cursor).
 */
template<typename Data>
lager::cursor<KisCurveOptionDataCommon> curveCursor(KisPaintOpOptionState<Data> *state)
{
    return state->cursor().zoom(kislager::lenses::to_base<KisCurveOptionDataCommon>);
}

/**
 * Bake function for curve options created without an enabled link or a
 * strength range override. It applies the same rules as
 * KisCurveOptionModel::bakedOptionData().
 */
template<typename Data>
Data bakeCurveOption(const Data &data)
{
    Data result = data;
    static_cast<KisCurveOptionDataCommon &>(result) =
        KisCurveOptionModel::bakeOptionData(data, true, std::make_tuple(data.strengthMinValue, data.strengthMaxValue));
    return result;
}

/**
 * Bake for a curve option created with an enabled link, e.g. Lightness
 * Strength, which applies only in the brush tip's lightness mode. The option
 * is written disabled while \p externallyEnabled is false.
 */
template<typename Data>
Data bakeLinkedCurveOption(const Data &data, bool externallyEnabled)
{
    Data result = data;
    static_cast<KisCurveOptionDataCommon &>(result) =
        KisCurveOptionModel::bakeOptionData(data,
                                            externallyEnabled,
                                            std::make_tuple(data.strengthMinValue, data.strengthMaxValue));
    return result;
}

} // namespace KisPaintOpOptionStateUtils

#endif // KISPAINTOPOPTIONSTATEUTILS_H
