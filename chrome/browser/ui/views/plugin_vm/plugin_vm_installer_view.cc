// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/plugin_vm/plugin_vm_installer_view.h"

#include <memory>

#include "base/optional.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/chromeos/plugin_vm/plugin_vm_installer_factory.h"
#include "chrome/browser/chromeos/plugin_vm/plugin_vm_manager.h"
#include "chrome/browser/chromeos/plugin_vm/plugin_vm_metrics_util.h"
#include "chrome/browser/chromeos/plugin_vm/plugin_vm_util.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/grit/chrome_unscaled_resources.h"
#include "chrome/grit/generated_resources.h"
#include "content/public/browser/browser_thread.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/time_format.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/chromeos/devicetype_utils.h"
#include "ui/strings/grit/ui_strings.h"
#include "ui/views/border.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/progress_bar.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view_class_properties.h"

namespace {

PluginVmInstallerView* g_plugin_vm_installer_view = nullptr;

constexpr gfx::Insets kButtonRowInsets(0, 64, 32, 64);
constexpr int kWindowWidth = 768;
constexpr int kWindowHeight = 636;

base::Optional<double> GetFractionComplete(double units_processed,
                                           double total_units) {
  if (total_units <= 0)
    return base::nullopt;
  double fraction_complete = units_processed / total_units;
  if (fraction_complete < 0.0 || fraction_complete > 1.0)
    return base::nullopt;
  return base::make_optional(fraction_complete);
}

}  // namespace

void plugin_vm::ShowPluginVmInstallerView(Profile* profile) {
  if (!g_plugin_vm_installer_view) {
    g_plugin_vm_installer_view = new PluginVmInstallerView(profile);
    views::DialogDelegate::CreateDialogWidget(g_plugin_vm_installer_view,
                                              nullptr, nullptr);
  }
  g_plugin_vm_installer_view->SetButtonRowInsets(kButtonRowInsets);
  g_plugin_vm_installer_view->GetWidget()->Show();
}

PluginVmInstallerView::PluginVmInstallerView(Profile* profile)
    : profile_(profile),
      plugin_vm_installer_(
          plugin_vm::PluginVmInstallerFactory::GetForProfile(profile)) {
  // Layout constants from the spec.
  gfx::Insets kDialogInsets(60, 64, 0, 64);
  constexpr gfx::Insets kLowerContainerInsets(12, 0, 52, 0);
  constexpr gfx::Size kLogoImageSize(32, 32);
  constexpr gfx::Size kBigImageSize(264, 264);
  constexpr int kTitleFontSize = 28;
  const gfx::FontList kTitleFont({"Google Sans"}, gfx::Font::NORMAL,
                                 kTitleFontSize, gfx::Font::Weight::NORMAL);
  constexpr int kTitleHeight = 64;
  constexpr int kMessageFontSize = 13;
  const gfx::FontList kMessageFont({"Roboto"}, gfx::Font::NORMAL,
                                   kMessageFontSize, gfx::Font::Weight::NORMAL);
  constexpr int kMessageHeight = 32;
  constexpr int kDownloadProgressMessageFontSize = 12;
  const gfx::FontList kDownloadProgressMessageFont(
      {"Roboto"}, gfx::Font::NORMAL, kDownloadProgressMessageFontSize,
      gfx::Font::Weight::NORMAL);
  constexpr int kDownloadProgressMessageHeight = 24;
  constexpr int kProgressBarHeight = 5;
  constexpr int kProgressBarTopMargin = 32;

  // Removed margins so dialog insets specify it instead.
  set_margins(gfx::Insets());

  views::BoxLayout* layout =
      SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kVertical, kDialogInsets));

  views::View* upper_container_view = new views::View();
  upper_container_view->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets()));
  AddChildView(upper_container_view);

  views::View* lower_container_view = new views::View();
  views::BoxLayout* lower_container_layout =
      lower_container_view->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kVertical, kLowerContainerInsets));
  AddChildView(lower_container_view);

  views::ImageView* logo_image = new views::ImageView();
  logo_image->SetImageSize(kLogoImageSize);
  logo_image->SetImage(
      ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
          IDR_LOGO_PLUGIN_VM_DEFAULT_32));
  logo_image->SetHorizontalAlignment(views::ImageView::Alignment::kLeading);
  upper_container_view->AddChildView(logo_image);

  big_message_label_ = new views::Label(GetBigMessage(), {kTitleFont});
  big_message_label_->SetProperty(
      views::kMarginsKey, gfx::Insets(kTitleHeight - kTitleFontSize, 0, 0, 0));
  big_message_label_->SetMultiLine(false);
  big_message_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  upper_container_view->AddChildView(big_message_label_);

  views::View* message_container_view = new views::View();
  views::BoxLayout* message_container_layout =
      message_container_view->SetLayoutManager(
          std::make_unique<views::BoxLayout>(
              views::BoxLayout::Orientation::kHorizontal,
              gfx::Insets(kMessageHeight - kMessageFontSize, 0, 0, 0)));
  upper_container_view->AddChildView(message_container_view);

  message_label_ = new views::Label(GetMessage(), {kMessageFont});
  message_label_->SetMultiLine(true);
  message_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  message_container_view->AddChildView(message_label_);

  time_left_message_label_ = new views::Label(base::string16(), {kMessageFont});
  time_left_message_label_->SetEnabledColor(gfx::kGoogleGrey700);
  time_left_message_label_->SetMultiLine(false);
  time_left_message_label_->SetHorizontalAlignment(gfx::ALIGN_RIGHT);
  message_container_view->AddChildView(time_left_message_label_);
  message_container_layout->SetFlexForView(time_left_message_label_, 1);

  progress_bar_ = new views::ProgressBar(kProgressBarHeight);
  progress_bar_->SetProperty(
      views::kMarginsKey,
      gfx::Insets(kProgressBarTopMargin - kProgressBarHeight, 0, 0, 0));
  upper_container_view->AddChildView(progress_bar_);

  download_progress_message_label_ =
      new views::Label(base::string16(), {kDownloadProgressMessageFont});
  download_progress_message_label_->SetEnabledColor(gfx::kGoogleGrey700);
  download_progress_message_label_->SetProperty(
      views::kMarginsKey, gfx::Insets(kDownloadProgressMessageHeight -
                                          kDownloadProgressMessageFontSize,
                                      0, 0, 0));
  download_progress_message_label_->SetMultiLine(false);
  download_progress_message_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  upper_container_view->AddChildView(download_progress_message_label_);

  big_image_ = new views::ImageView();
  big_image_->SetImageSize(kBigImageSize);
  big_image_->SetImage(
      ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
          IDR_PLUGIN_VM_INSTALLER));
  lower_container_view->AddChildView(big_image_);

  // Make sure the lower_container_view is pinned to the bottom of the dialog.
  lower_container_layout->set_main_axis_alignment(
      views::BoxLayout::MainAxisAlignment::kEnd);
  layout->SetFlexForView(lower_container_view, 1, true);
}

// static
PluginVmInstallerView* PluginVmInstallerView::GetActiveViewForTesting() {
  return g_plugin_vm_installer_view;
}

bool PluginVmInstallerView::ShouldShowWindowTitle() const {
  return false;
}

bool PluginVmInstallerView::Accept() {
  if (state_ == State::FINISHED) {
    // Launch button has been clicked.
    plugin_vm::PluginVmManager::GetForProfile(profile_)->LaunchPluginVm();
    return true;
  }
  DCHECK_EQ(state_, State::ERROR);
  // Retry button has been clicked to retry setting of PluginVm environment
  // after error occurred.
  StartInstallation();
  return false;
}

bool PluginVmInstallerView::Cancel() {
  switch (state_) {
    case State::STARTING:
    case State::DOWNLOADING_DLC:
      plugin_vm::RecordPluginVmSetupResultHistogram(
          plugin_vm::PluginVmSetupResult::kUserCancelledDownloadingPluginVmDlc);
      break;
    case State::DOWNLOADING:
      plugin_vm::RecordPluginVmSetupResultHistogram(
          plugin_vm::PluginVmSetupResult::
              kUserCancelledDownloadingPluginVmImage);
      break;
    case State::IMPORTING:
      plugin_vm::RecordPluginVmSetupResultHistogram(
          plugin_vm::PluginVmSetupResult::kUserCancelledImportingPluginVmImage);
      break;
    case State::ERROR:
      return true;
    default:
      NOTREACHED();
  }

  plugin_vm_installer_->Cancel();

  return true;
}

gfx::Size PluginVmInstallerView::CalculatePreferredSize() const {
  return gfx::Size(kWindowWidth, kWindowHeight);
}

void PluginVmInstallerView::OnVmExists() {
  // This case should only occur if the user manually installed a VM via vmc,
  // which is rare enough so we just re-use the regular success strings.
  DCHECK_EQ(state_, State::DOWNLOADING_DLC);
  state_ = State::FINISHED;
  OnStateUpdated();

  plugin_vm::RecordPluginVmSetupResultHistogram(
      plugin_vm::PluginVmSetupResult::kVmAlreadyExists);
  plugin_vm::RecordPluginVmSetupTimeHistogram(base::TimeTicks::Now() -
                                              setup_start_tick_);
}

void PluginVmInstallerView::OnDlcDownloadProgressUpdated(
    double progress,
    base::TimeDelta elapsed_time) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::DOWNLOADING_DLC);

  UpdateOperationProgress(progress * 100, 100.0, elapsed_time);
}

void PluginVmInstallerView::OnDlcDownloadCompleted() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::DOWNLOADING_DLC);

  state_ = State::DOWNLOADING;
  OnStateUpdated();
}

// TODO(timloh): Cancelling the installation immediately closes the dialog, but
// getting back to a clean state could take several seconds. If a user then
// re-opens the dialog, it could cause it to fail unexpectedly. We should make
// use of these callback to avoid this (and possibly merge them into a single
// callback).
void PluginVmInstallerView::OnDlcDownloadCancelled() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

void PluginVmInstallerView::OnDownloadProgressUpdated(
    uint64_t bytes_downloaded,
    int64_t content_length,
    base::TimeDelta elapsed_time) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::DOWNLOADING);

  download_progress_message_label_->SetText(
      GetDownloadProgressMessage(bytes_downloaded, content_length));
  download_progress_message_label_->NotifyAccessibilityEvent(
      ax::mojom::Event::kTextChanged, true);
  UpdateOperationProgress(bytes_downloaded, content_length, elapsed_time);
}

void PluginVmInstallerView::OnDownloadCompleted() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::DOWNLOADING);

  state_ = State::IMPORTING;
  OnStateUpdated();
}

void PluginVmInstallerView::OnDownloadCancelled() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

void PluginVmInstallerView::OnDownloadFailed(
    plugin_vm::PluginVmInstaller::FailureReason reason) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  state_ = State::ERROR;
  reason_ = reason;
  OnStateUpdated();

  plugin_vm::RecordPluginVmSetupResultHistogram(
      plugin_vm::PluginVmSetupResult::kErrorDownloadingPluginVmImage);
}

void PluginVmInstallerView::OnImportProgressUpdated(
    int percent_completed,
    base::TimeDelta elapsed_time) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::IMPORTING);

  UpdateOperationProgress(percent_completed, 100.0, elapsed_time);
}

void PluginVmInstallerView::OnImportCancelled() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

void PluginVmInstallerView::OnImportFailed(
    plugin_vm::PluginVmInstaller::FailureReason reason) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  state_ = State::ERROR;
  reason_ = reason;
  OnStateUpdated();

  plugin_vm::RecordPluginVmSetupResultHistogram(
      plugin_vm::PluginVmSetupResult::kErrorImportingPluginVmImage);
}

void PluginVmInstallerView::OnImported() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::IMPORTING);

  state_ = State::FINISHED;
  OnStateUpdated();

  plugin_vm::RecordPluginVmSetupResultHistogram(
      plugin_vm::PluginVmSetupResult::kSuccess);
  plugin_vm::RecordPluginVmSetupTimeHistogram(base::TimeTicks::Now() -
                                              setup_start_tick_);
}

base::string16 PluginVmInstallerView::GetBigMessage() const {
  switch (state_) {
    case State::STARTING:
    case State::DOWNLOADING_DLC:
    case State::DOWNLOADING:
    case State::IMPORTING:
      return l10n_util::GetStringUTF16(
          IDS_PLUGIN_VM_INSTALLER_ENVIRONMENT_SETTING_TITLE);
    case State::FINISHED:
      return l10n_util::GetStringUTF16(IDS_PLUGIN_VM_INSTALLER_FINISHED_TITLE);
    case State::ERROR:
      DCHECK(reason_);
      switch (*reason_) {
        case plugin_vm::PluginVmInstaller::FailureReason::NOT_ALLOWED:
          return l10n_util::GetStringUTF16(
              IDS_PLUGIN_VM_INSTALLER_NOT_ALLOWED_TITLE);
        default:
          return l10n_util::GetStringUTF16(IDS_PLUGIN_VM_INSTALLER_ERROR_TITLE);
      }
  }
}

base::string16 PluginVmInstallerView::GetMessage() const {
  switch (state_) {
    case State::STARTING:
    case State::DOWNLOADING_DLC:
      return l10n_util::GetStringUTF16(
          IDS_PLUGIN_VM_INSTALLER_START_DOWNLOADING_MESSAGE);
    case State::DOWNLOADING:
      return l10n_util::GetStringUTF16(
          IDS_PLUGIN_VM_INSTALLER_DOWNLOADING_MESSAGE);
    case State::IMPORTING:
      return l10n_util::GetStringUTF16(
          IDS_PLUGIN_VM_INSTALLER_IMPORTING_MESSAGE);
    case State::FINISHED:
      return l10n_util::GetStringUTF16(
          IDS_PLUGIN_VM_INSTALLER_FINISHED_MESSAGE);
    case State::ERROR:
      using Reason = plugin_vm::PluginVmInstaller::FailureReason;
      DCHECK(reason_);
      switch (*reason_) {
        default:
        case Reason::SIGNAL_NOT_CONNECTED:
        case Reason::OPERATION_IN_PROGRESS:
        case Reason::UNEXPECTED_DISK_IMAGE_STATUS:
        case Reason::INVALID_DISK_IMAGE_STATUS_RESPONSE:
        case Reason::DISPATCHER_NOT_AVAILABLE:
        case Reason::CONCIERGE_NOT_AVAILABLE:
          return l10n_util::GetStringFUTF16(
              IDS_PLUGIN_VM_INSTALLER_ERROR_MESSAGE_LOGIC_ERROR,
              base::NumberToString16(
                  static_cast<std::underlying_type_t<Reason>>(*reason_)));
        case Reason::NOT_ALLOWED:
          return l10n_util::GetStringUTF16(
              IDS_PLUGIN_VM_INSTALLER_NOT_ALLOWED_MESSAGE);
        case Reason::INVALID_IMAGE_URL:
        case Reason::HASH_MISMATCH:
          return l10n_util::GetStringFUTF16(
              IDS_PLUGIN_VM_INSTALLER_ERROR_MESSAGE_CONFIG_ERROR,
              base::NumberToString16(
                  static_cast<std::underlying_type_t<Reason>>(*reason_)));
        case Reason::DOWNLOAD_FAILED_UNKNOWN:
        case Reason::DOWNLOAD_FAILED_NETWORK:
        case Reason::DOWNLOAD_FAILED_ABORTED:
          return l10n_util::GetStringFUTF16(
              IDS_PLUGIN_VM_INSTALLER_ERROR_MESSAGE_DOWNLOAD_FAILED,
              base::NumberToString16(
                  static_cast<std::underlying_type_t<Reason>>(*reason_)));
        case Reason::COULD_NOT_OPEN_IMAGE:
        case Reason::INVALID_IMPORT_RESPONSE:
        case Reason::IMAGE_IMPORT_FAILED:
          return l10n_util::GetStringFUTF16(
              IDS_PLUGIN_VM_INSTALLER_ERROR_MESSAGE_INSTALLING_FAILED,
              base::NumberToString16(
                  static_cast<std::underlying_type_t<Reason>>(*reason_)));
      }
  }
}

void PluginVmInstallerView::SetFinishedCallbackForTesting(
    base::OnceCallback<void(bool success)> callback) {
  finished_callback_for_testing_ = std::move(callback);
}

PluginVmInstallerView::~PluginVmInstallerView() {
  plugin_vm_installer_->RemoveObserver();
  g_plugin_vm_installer_view = nullptr;
}

int PluginVmInstallerView::GetCurrentDialogButtons() const {
  switch (state_) {
    case State::STARTING:
    case State::DOWNLOADING_DLC:
    case State::DOWNLOADING:
    case State::IMPORTING:
      return ui::DIALOG_BUTTON_CANCEL;
    case State::FINISHED:
      return ui::DIALOG_BUTTON_OK;
    case State::ERROR:
      DCHECK(reason_);
      switch (*reason_) {
        case plugin_vm::PluginVmInstaller::FailureReason::NOT_ALLOWED:
          return ui::DIALOG_BUTTON_CANCEL;
        default:
          return ui::DIALOG_BUTTON_CANCEL | ui::DIALOG_BUTTON_OK;
      }
  }
}

base::string16 PluginVmInstallerView::GetCurrentDialogButtonLabel(
    ui::DialogButton button) const {
  switch (state_) {
    case State::STARTING:
    case State::DOWNLOADING_DLC:
    case State::DOWNLOADING:
    case State::IMPORTING: {
      DCHECK_EQ(button, ui::DIALOG_BUTTON_CANCEL);
      return l10n_util::GetStringUTF16(IDS_APP_CANCEL);
    }
    case State::FINISHED: {
      DCHECK_EQ(button, ui::DIALOG_BUTTON_OK);
      return l10n_util::GetStringUTF16(IDS_PLUGIN_VM_INSTALLER_LAUNCH_BUTTON);
    }
    case State::ERROR: {
      DCHECK(reason_);
      switch (*reason_) {
        case plugin_vm::PluginVmInstaller::FailureReason::NOT_ALLOWED:
          DCHECK_EQ(button, ui::DIALOG_BUTTON_CANCEL);
          return l10n_util::GetStringUTF16(IDS_APP_CANCEL);
        default:
          return l10n_util::GetStringUTF16(
              button == ui::DIALOG_BUTTON_OK
                  ? IDS_PLUGIN_VM_INSTALLER_RETRY_BUTTON
                  : IDS_APP_CANCEL);
      }
    }
  }
}

void PluginVmInstallerView::AddedToWidget() {
  // Defensive check that ensures an error message is shown if this
  // dialogue is reached somehow although PluginVm has been disabled.
  if (!plugin_vm::IsPluginVmAllowedForProfile(profile_)) {
    LOG(ERROR) << "PluginVm is disallowed by policy. Showing error screen.";
    state_ = State::ERROR;
    reason_ = plugin_vm::PluginVmInstaller::FailureReason::NOT_ALLOWED;
    plugin_vm::RecordPluginVmSetupResultHistogram(
        plugin_vm::PluginVmSetupResult::kPluginVmIsNotAllowed);
  }

  if (state_ == State::STARTING)
    StartInstallation();
  else
    OnStateUpdated();
}

void PluginVmInstallerView::OnStateUpdated() {
  SetBigMessageLabel();
  SetMessageLabel();
  SetBigImage();

  int buttons = GetCurrentDialogButtons();
  DialogDelegate::set_buttons(buttons);
  if (buttons & ui::DIALOG_BUTTON_OK) {
    DialogDelegate::set_button_label(
        ui::DIALOG_BUTTON_OK,
        GetCurrentDialogButtonLabel(ui::DIALOG_BUTTON_OK));
  }
  if (buttons & ui::DIALOG_BUTTON_CANCEL) {
    DialogDelegate::set_button_label(
        ui::DIALOG_BUTTON_CANCEL,
        GetCurrentDialogButtonLabel(ui::DIALOG_BUTTON_CANCEL));
  }

  const bool progress_bar_visible =
      state_ == State::STARTING || state_ == State::DOWNLOADING_DLC ||
      state_ == State::DOWNLOADING || state_ == State::IMPORTING;
  progress_bar_->SetVisible(progress_bar_visible);
  // Values outside the range [0,1] display an infinite loading animation.
  progress_bar_->SetValue(-1);

  // This will be shown once we receive download/import progress messages.
  time_left_message_label_->SetVisible(false);

  const bool download_progress_message_label_visible =
      state_ == State::DOWNLOADING;
  download_progress_message_label_->SetVisible(
      download_progress_message_label_visible);

  DialogModelChanged();
  GetWidget()->GetRootView()->Layout();

  if (state_ == State::FINISHED || state_ == State::ERROR) {
    if (finished_callback_for_testing_)
      std::move(finished_callback_for_testing_).Run(state_ == State::FINISHED);
  }
}

base::string16 PluginVmInstallerView::GetDownloadProgressMessage(
    uint64_t bytes_downloaded,
    int64_t content_length) const {
  DCHECK_EQ(state_, State::DOWNLOADING);

  base::Optional<double> fraction_complete =
      GetFractionComplete(bytes_downloaded, content_length);

  // If download size isn't known |fraction_complete| should be empty.
  if (fraction_complete.has_value()) {
    return l10n_util::GetStringFUTF16(
        IDS_PLUGIN_VM_INSTALLER_DOWNLOAD_PROGRESS_MESSAGE,
        ui::FormatBytesWithUnits(bytes_downloaded, ui::DATA_UNITS_GIBIBYTE,
                                 /*show_units=*/false),
        ui::FormatBytesWithUnits(content_length, ui::DATA_UNITS_GIBIBYTE,
                                 /*show_units=*/true));
  } else {
    return l10n_util::GetStringFUTF16(
        IDS_PLUGIN_VM_INSTALLER_DOWNLOAD_PROGRESS_WITHOUT_DOWNLOAD_SIZE_MESSAGE,
        ui::FormatBytesWithUnits(bytes_downloaded, ui::DATA_UNITS_GIBIBYTE,
                                 /*show_units=*/true));
  }
}

void PluginVmInstallerView::UpdateOperationProgress(
    double units_processed,
    double total_units,
    base::TimeDelta elapsed_time) const {
  DCHECK(state_ == State::DOWNLOADING_DLC || state_ == State::DOWNLOADING ||
         state_ == State::IMPORTING);

  base::Optional<double> maybe_fraction_complete =
      GetFractionComplete(units_processed, total_units);

  if (!maybe_fraction_complete || units_processed == 0 ||
      elapsed_time.is_zero()) {
    progress_bar_->SetValue(-1);
    time_left_message_label_->SetVisible(false);
    return;
  }

  const double fraction_complete = *maybe_fraction_complete;
  const double fraction_remaining = 1 - fraction_complete;

  progress_bar_->SetValue(fraction_complete);
  time_left_message_label_->SetVisible(true);
  base::TimeDelta remaining =
      fraction_remaining / fraction_complete * elapsed_time;
  time_left_message_label_->SetText(
      ui::TimeFormat::Simple(ui::TimeFormat::FORMAT_REMAINING,
                             ui::TimeFormat::LENGTH_SHORT, remaining));
  time_left_message_label_->NotifyAccessibilityEvent(
      ax::mojom::Event::kTextChanged, true);
}

void PluginVmInstallerView::SetBigMessageLabel() {
  big_message_label_->SetText(GetBigMessage());
  big_message_label_->SetVisible(true);
  big_message_label_->NotifyAccessibilityEvent(ax::mojom::Event::kTextChanged,
                                               true);
}

void PluginVmInstallerView::SetMessageLabel() {
  message_label_->SetText(GetMessage());
  message_label_->SetVisible(true);
  message_label_->NotifyAccessibilityEvent(ax::mojom::Event::kTextChanged,
                                           true);
}

void PluginVmInstallerView::SetBigImage() {
  if (state_ == State::ERROR) {
    big_image_->SetImage(
        ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
            IDR_PLUGIN_VM_INSTALLER_ERROR));
    return;
  }
  big_image_->SetImage(
      ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
          IDR_PLUGIN_VM_INSTALLER));
}

void PluginVmInstallerView::StartInstallation() {
  // In each case setup starts from this function (when dialog is opened or
  // retry button is clicked).
  setup_start_tick_ = base::TimeTicks::Now();

  state_ = State::DOWNLOADING_DLC;
  OnStateUpdated();

  plugin_vm_installer_->SetObserver(this);
  plugin_vm_installer_->Start();
}
