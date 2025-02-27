// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/arc/accessibility/arc_accessibility_helper_bridge.h"

#include <utility>

#include "ash/public/cpp/shell_window_ids.h"
#include "ash/public/cpp/window_properties.h"
#include "ash/system/message_center/arc/arc_notification_surface.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/memory/singleton.h"
#include "chrome/browser/chromeos/arc/accessibility/arc_accessibility_util.h"
#include "chrome/browser/chromeos/arc/accessibility/geometry_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/app_list/arc/arc_app_list_prefs_factory.h"
#include "chrome/common/extensions/api/accessibility_private.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/pref_names_util.h"
#include "chromeos/constants/chromeos_features.h"
#include "components/arc/arc_browser_context_keyed_service_factory_base.h"
#include "components/arc/arc_service_manager.h"
#include "components/arc/arc_util.h"
#include "components/arc/session/arc_bridge_service.h"
#include "components/exo/input_method_surface.h"
#include "components/exo/shell_surface.h"
#include "components/exo/shell_surface_util.h"
#include "components/exo/surface.h"
#include "components/exo/wm_helper.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "ui/accessibility/ax_action_data.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/window.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/widget/widget.h"

using ash::ArcNotificationSurface;
using ash::ArcNotificationSurfaceManager;

namespace {

exo::Surface* GetArcSurface(const aura::Window* window) {
  if (!window)
    return nullptr;

  exo::Surface* arc_surface = exo::Surface::AsSurface(window);
  if (!arc_surface)
    arc_surface = exo::GetShellMainSurface(window);
  return arc_surface;
}

void DispatchFocusChange(arc::mojom::AccessibilityNodeInfoData* node_data,
                         Profile* profile) {
  chromeos::AccessibilityManager* accessibility_manager =
      chromeos::AccessibilityManager::Get();
  if (!node_data || !accessibility_manager ||
      accessibility_manager->profile() != profile)
    return;

  exo::WMHelper* wm_helper = exo::WMHelper::GetInstance();
  if (!wm_helper)
    return;

  aura::Window* active_window = wm_helper->GetActiveWindow();
  if (!active_window)
    return;

  gfx::Rect bounds_in_screen = gfx::ToEnclosingRect(arc::ToChromeBounds(
      node_data->bounds_in_screen, wm_helper,
      views::Widget::GetWidgetForNativeView(active_window)));

  accessibility_manager->OnViewFocusedInArc(bounds_in_screen);
}

}  // namespace

namespace arc {

namespace {

// Singleton factory for ArcAccessibilityHelperBridge.
class ArcAccessibilityHelperBridgeFactory
    : public internal::ArcBrowserContextKeyedServiceFactoryBase<
          ArcAccessibilityHelperBridge,
          ArcAccessibilityHelperBridgeFactory> {
 public:
  // Factory name used by ArcBrowserContextKeyedServiceFactoryBase.
  static constexpr const char* kName = "ArcAccessibilityHelperBridgeFactory";

  static ArcAccessibilityHelperBridgeFactory* GetInstance() {
    return base::Singleton<ArcAccessibilityHelperBridgeFactory>::get();
  }

 protected:
  bool ServiceIsCreatedWithBrowserContext() const override { return true; }

 private:
  friend struct base::DefaultSingletonTraits<
      ArcAccessibilityHelperBridgeFactory>;

  ArcAccessibilityHelperBridgeFactory() {
    // ArcAccessibilityHelperBridge needs to track task creation and
    // destruction in the container, which are notified to ArcAppListPrefs
    // via Mojo.
    DependsOn(ArcAppListPrefsFactory::GetInstance());

    // ArcAccessibilityHelperBridge needs to track visibility change of Android
    // keyboard to delete its accessibility tree when it becomes hidden.
    DependsOn(ArcInputMethodManagerService::GetFactory());
  }
  ~ArcAccessibilityHelperBridgeFactory() override = default;
};

static constexpr const char* kTextShadowRaised =
    "-2px -2px 4px rgba(0, 0, 0, 0.5)";
static constexpr const char* kTextShadowDepressed =
    "2px 2px 4px rgba(0, 0, 0, 0.5)";
static constexpr const char* kTextShadowUniform =
    "-1px 0px 0px black, 0px -1px 0px black, 1px 0px 0px black, 0px  1px 0px "
    "black";
static constexpr const char* kTextShadowDropShadow =
    "0px 0px 2px rgba(0, 0, 0, 0.5), 2px 2px 2px black";

std::string GetARGBFromPrefs(PrefService* prefs,
                             const char* color_pref_name,
                             const char* opacity_pref_name) {
  const std::string color = prefs->GetString(color_pref_name);
  if (color.empty()) {
    return "";
  }
  const int opacity = prefs->GetInteger(opacity_pref_name);
  return base::StringPrintf("rgba(%s,%s)", color.c_str(),
                            base::NumberToString(opacity / 100.0).c_str());
}

ArcAccessibilityHelperBridge::TreeKey KeyForTaskId(int32_t value) {
  return {ArcAccessibilityHelperBridge::TreeKeyType::kTaskId, value, {}};
}

ArcAccessibilityHelperBridge::TreeKey KeyForNotification(std::string value) {
  return {ArcAccessibilityHelperBridge::TreeKeyType::kNotificationKey, 0,
          std::move(value)};
}

ArcAccessibilityHelperBridge::TreeKey KeyForInputMethod() {
  return {ArcAccessibilityHelperBridge::TreeKeyType::kInputMethod, 0, {}};
}

}  // namespace

arc::mojom::CaptionStylePtr GetCaptionStyleFromPrefs(PrefService* prefs) {
  DCHECK(prefs);

  arc::mojom::CaptionStylePtr style = arc::mojom::CaptionStyle::New();

  style->text_size = prefs->GetString(prefs::kAccessibilityCaptionsTextSize);
  style->text_color =
      GetARGBFromPrefs(prefs, prefs::kAccessibilityCaptionsTextColor,
                       prefs::kAccessibilityCaptionsTextOpacity);
  style->background_color =
      GetARGBFromPrefs(prefs, prefs::kAccessibilityCaptionsBackgroundColor,
                       prefs::kAccessibilityCaptionsBackgroundOpacity);
  style->user_locale = prefs->GetString(language::prefs::kApplicationLocale);

  const std::string test_shadow =
      prefs->GetString(prefs::kAccessibilityCaptionsTextShadow);
  if (test_shadow == kTextShadowRaised) {
    style->text_shadow_type = arc::mojom::CaptionTextShadowType::RAISED;
  } else if (test_shadow == kTextShadowDepressed) {
    style->text_shadow_type = arc::mojom::CaptionTextShadowType::DEPRESSED;
  } else if (test_shadow == kTextShadowUniform) {
    style->text_shadow_type = arc::mojom::CaptionTextShadowType::UNIFORM;
  } else if (test_shadow == kTextShadowDropShadow) {
    style->text_shadow_type = arc::mojom::CaptionTextShadowType::DROP_SHADOW;
  } else {
    style->text_shadow_type = arc::mojom::CaptionTextShadowType::NONE;
  }

  return style;
}

// static
void ArcAccessibilityHelperBridge::CreateFactory() {
  ArcAccessibilityHelperBridgeFactory::GetInstance();
}

// static
ArcAccessibilityHelperBridge*
ArcAccessibilityHelperBridge::GetForBrowserContext(
    content::BrowserContext* context) {
  return ArcAccessibilityHelperBridgeFactory::GetForBrowserContext(context);
}

// static
ArcAccessibilityHelperBridge::TreeKey
ArcAccessibilityHelperBridge::KeyForNotification(std::string notification_key) {
  return arc::KeyForNotification(std::move(notification_key));
}

// The list of prefs we want to observe.
const char* const kCaptionStylePrefsToObserve[] = {
    prefs::kAccessibilityCaptionsTextSize,
    prefs::kAccessibilityCaptionsTextFont,
    prefs::kAccessibilityCaptionsTextColor,
    prefs::kAccessibilityCaptionsTextOpacity,
    prefs::kAccessibilityCaptionsBackgroundColor,
    prefs::kAccessibilityCaptionsTextShadow,
    prefs::kAccessibilityCaptionsBackgroundOpacity,
    language::prefs::kApplicationLocale};

ArcAccessibilityHelperBridge::ArcAccessibilityHelperBridge(
    content::BrowserContext* browser_context,
    ArcBridgeService* arc_bridge_service)
    : profile_(Profile::FromBrowserContext(browser_context)),
      arc_bridge_service_(arc_bridge_service) {
  pref_change_registrar_ = std::make_unique<PrefChangeRegistrar>();
  pref_change_registrar_->Init(
      Profile::FromBrowserContext(browser_context)->GetPrefs());

  for (const char* const pref_name : kCaptionStylePrefsToObserve) {
    pref_change_registrar_->Add(
        pref_name,
        base::Bind(&ArcAccessibilityHelperBridge::UpdateCaptionSettings,
                   base::Unretained(this)));
  }

  arc_bridge_service_->accessibility_helper()->SetHost(this);
  arc_bridge_service_->accessibility_helper()->AddObserver(this);

  // Null on testing.
  auto* app_list_prefs = ArcAppListPrefs::Get(profile_);
  if (app_list_prefs)
    app_list_prefs->AddObserver(this);

  auto* arc_ime_service =
      ArcInputMethodManagerService::GetForBrowserContext(browser_context);
  if (arc_ime_service)
    arc_ime_service->AddObserver(this);
}

ArcAccessibilityHelperBridge::~ArcAccessibilityHelperBridge() = default;

void ArcAccessibilityHelperBridge::SetNativeChromeVoxArcSupport(bool enabled) {
  aura::Window* window = GetActiveWindow();
  if (!window)
    return;
  int32_t task_id = arc::GetWindowTaskId(window);
  if (task_id == kNoTaskId)
    return;

  std::unique_ptr<aura::WindowTracker> window_tracker =
      std::make_unique<aura::WindowTracker>();
  window_tracker->Add(window);

  auto* instance =
      ARC_GET_INSTANCE_FOR_METHOD(arc_bridge_service_->accessibility_helper(),
                                  SetNativeChromeVoxArcSupportForFocusedWindow);
  instance->SetNativeChromeVoxArcSupportForFocusedWindow(
      enabled, base::BindOnce(&ArcAccessibilityHelperBridge::
                                  OnSetNativeChromeVoxArcSupportProcessed,
                              base::Unretained(this), std::move(window_tracker),
                              enabled));
}

void ArcAccessibilityHelperBridge::OnSetNativeChromeVoxArcSupportProcessed(
    std::unique_ptr<aura::WindowTracker> window_tracker,
    bool enabled,
    bool processed) {
  if (!processed || window_tracker->windows().size() != 1)
    return;

  aura::Window* window = window_tracker->Pop();
  int32_t task_id = arc::GetWindowTaskId(window);
  DCHECK_NE(task_id, kNoTaskId);

  if (!enabled) {
    trees_.erase(KeyForTaskId(task_id));

    exo::Surface* surface = exo::GetShellMainSurface(window);
    if (surface) {
      views::Widget* widget = views::Widget::GetWidgetForNativeWindow(window);
      static_cast<exo::ShellSurfaceBase*>(widget->widget_delegate())
          ->SetChildAxTreeId(ui::AXTreeIDUnknown());
    }
  }

  UpdateWindowProperties(window);
}

void ArcAccessibilityHelperBridge::Shutdown() {
  // We do not unregister ourselves from WMHelper as an ActivationObserver
  // because it is always null at this point during teardown.

  // Null on testing.
  auto* app_list_prefs = ArcAppListPrefs::Get(profile_);
  if (app_list_prefs)
    app_list_prefs->RemoveObserver(this);

  auto* arc_ime_service =
      ArcInputMethodManagerService::GetForBrowserContext(profile_);
  if (arc_ime_service)
    arc_ime_service->RemoveObserver(this);

  arc_bridge_service_->accessibility_helper()->RemoveObserver(this);
  arc_bridge_service_->accessibility_helper()->SetHost(nullptr);
}

void ArcAccessibilityHelperBridge::OnConnectionReady() {
  UpdateEnabledFeature();
  UpdateCaptionSettings();

  chromeos::AccessibilityManager* accessibility_manager =
      chromeos::AccessibilityManager::Get();
  if (accessibility_manager) {
    accessibility_status_subscription_ =
        accessibility_manager->RegisterCallback(base::BindRepeating(
            &ArcAccessibilityHelperBridge::OnAccessibilityStatusChanged,
            base::Unretained(this)));
    SetExploreByTouchEnabled(accessibility_manager->IsSpokenFeedbackEnabled());
  }

  auto* surface_manager = ArcNotificationSurfaceManager::Get();
  if (surface_manager)
    surface_manager->AddObserver(this);
}

void ArcAccessibilityHelperBridge::OnConnectionClosed() {
  auto* surface_manager = ArcNotificationSurfaceManager::Get();
  if (surface_manager)
    surface_manager->RemoveObserver(this);
}

void ArcAccessibilityHelperBridge::OnAccessibilityEvent(
    mojom::AccessibilityEventDataPtr event_data) {
  filter_type_ = GetFilterTypeForProfile(profile_);
  switch (filter_type_) {
    case arc::mojom::AccessibilityFilterType::ALL:
      HandleFilterTypeAllEvent(std::move(event_data));
      break;
    case arc::mojom::AccessibilityFilterType::FOCUS:
      HandleFilterTypeFocusEvent(std::move(event_data));
      break;
    case arc::mojom::AccessibilityFilterType::OFF:
      break;
  }
}

void ArcAccessibilityHelperBridge::OnNotificationStateChanged(
    const std::string& notification_key,
    arc::mojom::AccessibilityNotificationStateType state) {
  auto key = KeyForNotification(notification_key);
  switch (state) {
    case arc::mojom::AccessibilityNotificationStateType::SURFACE_CREATED: {
      if (GetFromKey(key))
        return;

      AXTreeSourceArc* tree_source = CreateFromKey(std::move(key));
      ui::AXTreeData tree_data;
      if (!tree_source->GetTreeData(&tree_data)) {
        NOTREACHED();
        return;
      }
      UpdateTreeIdOfNotificationSurface(notification_key, tree_data.tree_id);
      break;
    }
    case arc::mojom::AccessibilityNotificationStateType::SURFACE_REMOVED:
      trees_.erase(key);
      UpdateTreeIdOfNotificationSurface(notification_key,
                                        ui::AXTreeIDUnknown());
      break;
  }
}

void ArcAccessibilityHelperBridge::OnAction(
    const ui::AXActionData& data) const {
  DCHECK(data.target_node_id);

  AXTreeSourceArc* tree_source = GetFromTreeId(data.target_tree_id);
  if (!tree_source)
    return;

  if (data.action == ax::mojom::Action::kInternalInvalidateTree) {
    tree_source->InvalidateTree();
    return;
  }

  base::Optional<int32_t> window_id = tree_source->window_id();
  if (!window_id)
    return;

  const base::Optional<mojom::AccessibilityActionType> action =
      ConvertToAndroidAction(data.action);
  if (!action.has_value())
    return;

  arc::mojom::AccessibilityActionDataPtr action_data =
      arc::mojom::AccessibilityActionData::New();

  action_data->node_id = data.target_node_id;
  action_data->window_id = window_id.value();
  action_data->action_type = action.value();

  if (action == arc::mojom::AccessibilityActionType::GET_TEXT_LOCATION) {
    action_data->start_index = data.start_index;
    action_data->end_index = data.end_index;
    auto* instance = ARC_GET_INSTANCE_FOR_METHOD(
        arc_bridge_service_->accessibility_helper(), RefreshWithExtraData);
    if (!instance) {
      OnActionResult(data, false);
      return;
    }
    instance->RefreshWithExtraData(
        std::move(action_data),
        base::BindOnce(
            &ArcAccessibilityHelperBridge::OnGetTextLocationDataResult,
            base::Unretained(this), data));
    return;
  } else if (action == arc::mojom::AccessibilityActionType::CUSTOM_ACTION) {
    action_data->custom_action_id = data.custom_action_id;
  } else if (action == arc::mojom::AccessibilityActionType::SHOW_ON_SCREEN) {
    // This action is performed every time ChromeVox focus gets changed (from
    // Background.setCurrentRange). Use this action as a notification of focus
    // change, and update focus cache.
    tree_source->UpdateAccessibilityFocusLocation(data.target_node_id);
  }

  auto* instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->accessibility_helper(), PerformAction);
  if (!instance) {
    // TODO (b/146809329): This case should probably destroy all trees.
    OnActionResult(data, false);
    return;
  }

  instance->PerformAction(
      std::move(action_data),
      base::BindOnce(&ArcAccessibilityHelperBridge::OnActionResult,
                     base::Unretained(this), data));
}

void ArcAccessibilityHelperBridge::OnTaskDestroyed(int32_t task_id) {
  trees_.erase(KeyForTaskId(task_id));
}

void ArcAccessibilityHelperBridge::OnAndroidVirtualKeyboardVisibilityChanged(
    bool visible) {
  if (!visible)
    trees_.erase(KeyForInputMethod());
}

void ArcAccessibilityHelperBridge::OnNotificationSurfaceAdded(
    ArcNotificationSurface* surface) {
  const std::string& notification_key = surface->GetNotificationKey();

  auto* const tree = GetFromKey(KeyForNotification(notification_key));
  if (!tree)
    return;

  ui::AXTreeData tree_data;
  if (!tree->GetTreeData(&tree_data))
    return;

  surface->SetAXTreeId(tree_data.tree_id);

  // Dispatch ax::mojom::Event::kChildrenChanged to force AXNodeData of the
  // notification updated. As order of OnNotificationSurfaceAdded call is not
  // guaranteed, we are dispatching the event in both
  // ArcAccessibilityHelperBridge and ArcNotificationContentView. The event
  // needs to be dispatched after 1. ax tree id is set to the surface, 2 the
  // surface is attached to the content view.
  if (surface->IsAttached()) {
    surface->GetAttachedHost()->NotifyAccessibilityEvent(
        ax::mojom::Event::kChildrenChanged, false);
  }
}

void ArcAccessibilityHelperBridge::InvokeUpdateEnabledFeatureForTesting() {
  UpdateEnabledFeature();
}

aura::Window* ArcAccessibilityHelperBridge::GetActiveWindow() {
  exo::WMHelper* wm_helper = exo::WMHelper::GetInstance();
  if (!wm_helper)
    return nullptr;

  return wm_helper->GetActiveWindow();
}

extensions::EventRouter* ArcAccessibilityHelperBridge::GetEventRouter() const {
  return extensions::EventRouter::Get(profile_);
}

arc::mojom::AccessibilityFilterType
ArcAccessibilityHelperBridge::GetFilterTypeForProfile(Profile* profile) {
  chromeos::AccessibilityManager* accessibility_manager =
      chromeos::AccessibilityManager::Get();
  if (!accessibility_manager)
    return arc::mojom::AccessibilityFilterType::OFF;

  // TODO(yawano): Support the case where primary user is in background.
  if (accessibility_manager->profile() != profile)
    return arc::mojom::AccessibilityFilterType::OFF;

  if (accessibility_manager->IsSelectToSpeakEnabled() ||
      accessibility_manager->IsSwitchAccessEnabled() ||
      accessibility_manager->IsSpokenFeedbackEnabled()) {
    return arc::mojom::AccessibilityFilterType::ALL;
  }

  if (accessibility_manager->IsFocusHighlightEnabled())
    return arc::mojom::AccessibilityFilterType::FOCUS;
  return arc::mojom::AccessibilityFilterType::OFF;
}

void ArcAccessibilityHelperBridge::UpdateCaptionSettings() const {
  arc::mojom::CaptionStylePtr caption_style =
      GetCaptionStyleFromPrefs(profile_->GetPrefs());

  if (!caption_style)
    return;

  auto* instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->accessibility_helper(), SetCaptionStyle);

  if (!instance)
    return;

  instance->SetCaptionStyle(std::move(caption_style));
}

void ArcAccessibilityHelperBridge::OnWindowActivated(
    ActivationReason reason,
    aura::Window* gained_active,
    aura::Window* lost_active) {
  if (gained_active == lost_active)
    return;

  UpdateWindowProperties(gained_active);
}

void ArcAccessibilityHelperBridge::OnActionResult(const ui::AXActionData& data,
                                                  bool result) const {
  AXTreeSourceArc* tree_source = GetFromTreeId(data.target_tree_id);

  if (!tree_source)
    return;

  tree_source->NotifyActionResult(data, result);
}

void ArcAccessibilityHelperBridge::OnGetTextLocationDataResult(
    const ui::AXActionData& data,
    const base::Optional<gfx::Rect>& result_rect) const {
  AXTreeSourceArc* tree_source = GetFromTreeId(data.target_tree_id);

  if (!tree_source)
    return;

  tree_source->NotifyGetTextLocationDataResult(
      data, OnGetTextLocationDataResultInternal(result_rect));
}

base::Optional<gfx::Rect>
ArcAccessibilityHelperBridge::OnGetTextLocationDataResultInternal(
    const base::Optional<gfx::Rect>& result_rect) const {
  if (!result_rect)
    return base::nullopt;

  exo::WMHelper* wm_helper = exo::WMHelper::GetInstance();
  if (!wm_helper)
    return base::nullopt;

  aura::Window* active_window = wm_helper->GetActiveWindow();
  if (!active_window)
    return base::nullopt;

  gfx::RectF rect_f = arc::ToChromeScale(*result_rect, wm_helper);
  arc::ScaleDeviceFactor(rect_f, active_window->GetToplevelWindow());
  return gfx::ToEnclosingRect(rect_f);
}

void ArcAccessibilityHelperBridge::OnAccessibilityStatusChanged(
    const chromeos::AccessibilityStatusEventDetails& event_details) {
  if (event_details.notification_type !=
          chromeos::ACCESSIBILITY_TOGGLE_FOCUS_HIGHLIGHT &&
      event_details.notification_type !=
          chromeos::ACCESSIBILITY_TOGGLE_SELECT_TO_SPEAK &&
      event_details.notification_type !=
          chromeos::ACCESSIBILITY_TOGGLE_SPOKEN_FEEDBACK &&
      event_details.notification_type !=
          chromeos::ACCESSIBILITY_TOGGLE_SWITCH_ACCESS) {
    return;
  }

  UpdateEnabledFeature();
  UpdateWindowProperties(GetActiveWindow());

  if (event_details.notification_type ==
      chromeos::ACCESSIBILITY_TOGGLE_SPOKEN_FEEDBACK) {
    SetExploreByTouchEnabled(event_details.enabled);
  }
}

void ArcAccessibilityHelperBridge::UpdateEnabledFeature() {
  arc::mojom::AccessibilityFilterType new_filter_type_ =
      GetFilterTypeForProfile(profile_);
  // Clear trees when filter type is changed to non-ALL.
  if (filter_type_ != new_filter_type_ &&
      new_filter_type_ != arc::mojom::AccessibilityFilterType::ALL) {
    trees_.clear();
  }
  filter_type_ = new_filter_type_;

  auto* instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->accessibility_helper(), SetFilter);
  if (instance)
    instance->SetFilter(filter_type_);

  if (!chromeos::AccessibilityManager::Get())
    return;
  is_focus_highlight_enabled_ =
      filter_type_ != arc::mojom::AccessibilityFilterType::OFF &&
      chromeos::AccessibilityManager::Get()->IsFocusHighlightEnabled();

  bool add_activation_observer =
      filter_type_ == arc::mojom::AccessibilityFilterType::ALL;
  if (add_activation_observer == activation_observer_added_)
    return;

  if (!exo::WMHelper::HasInstance())
    return;

  exo::WMHelper* wm_helper = exo::WMHelper::GetInstance();
  if (add_activation_observer) {
    wm_helper->AddActivationObserver(this);
    activation_observer_added_ = true;
  } else {
    activation_observer_added_ = false;
    wm_helper->RemoveActivationObserver(this);
  }
}

void ArcAccessibilityHelperBridge::UpdateWindowProperties(
    aura::Window* window) {
  if (!window)
    return;

  if (!GetArcSurface(window))
    return;

  // First, do a lookup for the task id associated with this app. There should
  // always be a valid entry.
  int32_t task_id = arc::GetWindowTaskId(window);

  // Do a lookup for the tree source. A tree source may not exist because the
  // app isn't whitelisted Android side or no data has been received for the
  // app.
  auto* tree = GetFromKey(KeyForTaskId(task_id));
  bool use_talkback = !tree;

  window->SetProperty(aura::client::kAccessibilityTouchExplorationPassThrough,
                      use_talkback);
  window->SetProperty(ash::kSearchKeyAcceleratorReservedKey, use_talkback);
  window->SetProperty(aura::client::kAccessibilityFocusFallsbackToWidgetKey,
                      !use_talkback);
}

void ArcAccessibilityHelperBridge::SetExploreByTouchEnabled(bool enabled) {
  auto* instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->accessibility_helper(), SetExploreByTouchEnabled);
  if (instance)
    instance->SetExploreByTouchEnabled(enabled);
}

void ArcAccessibilityHelperBridge::UpdateTreeIdOfNotificationSurface(
    const std::string& notification_key,
    ui::AXTreeID tree_id) {
  auto* surface_manager = ArcNotificationSurfaceManager::Get();
  if (!surface_manager)
    return;

  ArcNotificationSurface* surface =
      surface_manager->GetArcSurface(notification_key);
  if (!surface)
    return;

  surface->SetAXTreeId(tree_id);

  if (surface->IsAttached()) {
    // Dispatch ax::mojom::Event::kChildrenChanged to force AXNodeData of the
    // notification updated.
    surface->GetAttachedHost()->NotifyAccessibilityEvent(
        ax::mojom::Event::kChildrenChanged, false);
  }
}

void ArcAccessibilityHelperBridge::HandleFilterTypeFocusEvent(
    mojom::AccessibilityEventDataPtr event_data) {
  if (event_data.get()->node_data.size() == 1 &&
      event_data->event_type ==
          arc::mojom::AccessibilityEventType::VIEW_FOCUSED)
    DispatchFocusChange(event_data.get()->node_data[0].get(), profile_);
}

void ArcAccessibilityHelperBridge::HandleFilterTypeAllEvent(
    mojom::AccessibilityEventDataPtr event_data) {
  if (event_data->event_type ==
      arc::mojom::AccessibilityEventType::ANNOUNCEMENT) {
    if (!event_data->eventText.has_value())
      return;

    extensions::EventRouter* event_router = GetEventRouter();
    std::unique_ptr<base::ListValue> event_args(
        extensions::api::accessibility_private::OnAnnounceForAccessibility::
            Create(*(event_data->eventText)));
    std::unique_ptr<extensions::Event> event(new extensions::Event(
        extensions::events::ACCESSIBILITY_PRIVATE_ON_ANNOUNCE_FOR_ACCESSIBILITY,
        extensions::api::accessibility_private::OnAnnounceForAccessibility::
            kEventName,
        std::move(event_args)));
    event_router->BroadcastEvent(std::move(event));
    return;
  }

  if (event_data->node_data.empty())
    return;

  AXTreeSourceArc* tree_source = nullptr;
  bool is_notification_event = event_data->notification_key.has_value();
  if (is_notification_event) {
    const std::string& notification_key = event_data->notification_key.value();

    // This bridge must receive OnNotificationStateChanged call for the
    // notification_key before this receives an accessibility event for it.
    tree_source = GetFromKey(KeyForNotification(notification_key));
    DCHECK(tree_source);
  } else if (event_data->is_input_method_window) {
    exo::InputMethodSurface* input_method_surface =
        exo::InputMethodSurface::GetInputMethodSurface();

    if (!input_method_surface)
      return;

    if (!trees_.count(KeyForInputMethod())) {
      auto* tree = CreateFromKey(KeyForInputMethod());
      ui::AXTreeData tree_data;
      tree->GetTreeData(&tree_data);
      input_method_surface->SetChildAxTreeId(tree_data.tree_id);
    }

    tree_source = GetFromKey(KeyForInputMethod());
  } else {
    aura::Window* active_window = GetActiveWindow();
    if (!active_window)
      return;

    auto task_id = arc::GetWindowTaskId(active_window);
    if (event_data->task_id != kNoTaskId) {
      // Event data has task ID. Check task ID.
      if (task_id != event_data->task_id)
        return;
    } else {
      // Event data does not have task ID. Check window ID instead.
      auto window_id = exo::GetShellClientAccessibilityId(active_window);
      if (window_id != event_data->window_id)
        return;
    }

    auto key = KeyForTaskId(task_id);
    tree_source = GetFromKey(key);

    if (!tree_source) {
      tree_source = CreateFromKey(key);

      ui::AXTreeData tree_data;
      tree_source->GetTreeData(&tree_data);
      exo::Surface* surface = exo::GetShellMainSurface(active_window);
      if (surface) {
        views::Widget* widget =
            views::Widget::GetWidgetForNativeWindow(active_window);
        static_cast<exo::ShellSurfaceBase*>(widget->widget_delegate())
            ->SetChildAxTreeId(tree_data.tree_id);
      }
    }
  }

  if (!tree_source)
    return;

  tree_source->NotifyAccessibilityEvent(event_data.get());

  if (is_notification_event &&
      event_data->event_type ==
          arc::mojom::AccessibilityEventType::VIEW_TEXT_SELECTION_CHANGED) {
    // If text selection changed event is dispatched from Android, it
    // means that user is trying to type a text in Android notification.
    // Dispatch text selection changed event to notification content view
    // as the view can take necessary actions, e.g. activate itself, etc.
    auto* surface_manager = ArcNotificationSurfaceManager::Get();
    if (surface_manager) {
      ArcNotificationSurface* surface =
          surface_manager->GetArcSurface(event_data->notification_key.value());
      if (surface) {
        surface->GetAttachedHost()->NotifyAccessibilityEvent(
            ax::mojom::Event::kTextSelectionChanged, true);
      }
    }
  } else if (!is_notification_event) {
    UpdateWindowProperties(GetActiveWindow());
  }

  if (is_focus_highlight_enabled_ &&
      event_data->event_type ==
          arc::mojom::AccessibilityEventType::VIEW_FOCUSED) {
    DispatchFocusChange(
        tree_source->GetFromId(event_data->source_id)->GetNode(), profile_);
  }
}

AXTreeSourceArc* ArcAccessibilityHelperBridge::CreateFromKey(TreeKey key) {
  auto tree = std::make_unique<AXTreeSourceArc>(this);
  auto* tree_ptr = tree.get();
  trees_.insert(std::make_pair(std::move(key), std::move(tree)));
  return tree_ptr;
}

AXTreeSourceArc* ArcAccessibilityHelperBridge::GetFromKey(const TreeKey& key) {
  auto tree_it = trees_.find(key);
  if (tree_it == trees_.end())
    return nullptr;

  return tree_it->second.get();
}

AXTreeSourceArc* ArcAccessibilityHelperBridge::GetFromTreeId(
    ui::AXTreeID tree_id) const {
  for (auto it = trees_.begin(); it != trees_.end(); ++it) {
    ui::AXTreeData tree_data;
    it->second->GetTreeData(&tree_data);
    if (tree_data.tree_id == tree_id)
      return it->second.get();
  }
  return nullptr;
}

}  // namespace arc
