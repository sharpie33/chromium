// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.ui;

import android.os.Bundle;
import android.os.CancellationSignal;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;

import org.chromium.base.Callback;
import org.chromium.base.TraceEvent;
import org.chromium.base.metrics.RecordUserAction;
import org.chromium.base.supplier.ObservableSupplier;
import org.chromium.base.supplier.Supplier;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ActivityTabProvider;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.MenuOrKeyboardActionController;
import org.chromium.chrome.browser.TabThemeColorProvider;
import org.chromium.chrome.browser.compositor.bottombar.OverlayPanel;
import org.chromium.chrome.browser.compositor.bottombar.OverlayPanelManager;
import org.chromium.chrome.browser.compositor.layouts.EmptyOverviewModeObserver;
import org.chromium.chrome.browser.compositor.layouts.LayoutManager;
import org.chromium.chrome.browser.compositor.layouts.OverviewModeBehavior;
import org.chromium.chrome.browser.directactions.DirectActionInitializer;
import org.chromium.chrome.browser.findinpage.FindToolbarManager;
import org.chromium.chrome.browser.findinpage.FindToolbarObserver;
import org.chromium.chrome.browser.flags.ActivityType;
import org.chromium.chrome.browser.lifecycle.Destroyable;
import org.chromium.chrome.browser.lifecycle.InflationObserver;
import org.chromium.chrome.browser.metrics.UkmRecorder;
import org.chromium.chrome.browser.share.ShareDelegate;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tabmodel.TabModelSelector;
import org.chromium.chrome.browser.toolbar.ToolbarManager;
import org.chromium.chrome.browser.toolbar.top.ToolbarControlContainer;
import org.chromium.chrome.browser.ui.appmenu.AppMenuBlocker;
import org.chromium.chrome.browser.ui.appmenu.AppMenuCoordinator;
import org.chromium.chrome.browser.ui.appmenu.AppMenuCoordinatorFactory;
import org.chromium.chrome.browser.ui.messages.snackbar.SnackbarManager;
import org.chromium.chrome.browser.vr.VrModuleProvider;
import org.chromium.chrome.browser.widget.ScrimView;
import org.chromium.chrome.browser.widget.bottomsheet.BottomSheetController;
import org.chromium.components.paintpreview.browser.PaintPreviewUtils;
import org.chromium.ui.base.DeviceFormFactor;
import org.chromium.ui.modaldialog.ModalDialogManager.ModalDialogManagerObserver;
import org.chromium.ui.modelutil.PropertyModel;
import org.chromium.ui.vr.VrModeObserver;

import java.util.function.Consumer;

/**
 * The root UI coordinator. This class will eventually be responsible for inflating and managing
 * lifecycle of the main UI components.
 *
 * The specific things this component will manage and how it will hook into Chrome*Activity are
 * still being discussed See https://crbug.com/931496.
 */
public class RootUiCoordinator
        implements Destroyable, InflationObserver,
                   MenuOrKeyboardActionController.MenuOrKeyboardActionHandler, AppMenuBlocker {
    protected ChromeActivity mActivity;
    protected @Nullable AppMenuCoordinator mAppMenuCoordinator;
    private final MenuOrKeyboardActionController mMenuOrKeyboardActionController;
    private ActivityTabProvider mActivityTabProvider;
    private ObservableSupplier<ShareDelegate> mShareDelegateSupplier;

    protected @Nullable FindToolbarManager mFindToolbarManager;
    private @Nullable FindToolbarObserver mFindToolbarObserver;

    private Callback<LayoutManager> mLayoutManagerSupplierCallback;
    private OverlayPanelManager mOverlayPanelManager;
    private OverlayPanelManager.OverlayPanelManagerObserver mOverlayPanelManagerObserver;

    private Callback<OverviewModeBehavior> mOverviewModeBehaviorSupplierCallback;
    private OverviewModeBehavior mOverviewModeBehavior;
    private OverviewModeBehavior.OverviewModeObserver mOverviewModeObserver;

    /** A means of providing the theme color to different features. */
    private TabThemeColorProvider mTabThemeColorProvider;
    @Nullable
    private Callback<Boolean> mOnOmniboxFocusChangedListener;
    protected ToolbarManager mToolbarManager;
    private ModalDialogManagerObserver mModalDialogManagerObserver;

    private VrModeObserver mVrModeObserver;

    private BottomSheetManager mBottomSheetManager;
    private BottomSheetController mBottomSheetController;
    private SnackbarManager mBottomSheetSnackbarManager;

    private ScrimView mScrimView;
    private DirectActionInitializer mDirectActionInitializer;

    /**
     * Create a new {@link RootUiCoordinator} for the given activity.
     * @param activity The containing {@link ChromeActivity}. TODO(https://crbug.com/931496):
     *         Remove this in favor of passing in direct dependencies.
     * @param onOmniboxFocusChangedListener Callback<Boolean> callback to invoke when Omnibox focus
     *         changes.
     */
    public RootUiCoordinator(ChromeActivity activity,
            @Nullable Callback<Boolean> onOmniboxFocusChangedListener,
            ObservableSupplier<ShareDelegate> shareDelegateSupplier) {
        mActivity = activity;
        mOnOmniboxFocusChangedListener = onOmniboxFocusChangedListener;
        mActivity.getLifecycleDispatcher().register(this);

        mMenuOrKeyboardActionController = mActivity.getMenuOrKeyboardActionController();
        mMenuOrKeyboardActionController.registerMenuOrKeyboardActionHandler(this);
        mActivityTabProvider = mActivity.getActivityTabProvider();

        mLayoutManagerSupplierCallback = this::onLayoutManagerAvailable;
        mActivity.getLayoutManagerSupplier().addObserver(mLayoutManagerSupplierCallback);

        mShareDelegateSupplier = shareDelegateSupplier;

        initOverviewModeSupplierObserver();
    }

    // TODO(pnoland, crbug.com/865801): remove this in favor of wiring it directly.
    public ToolbarManager getToolbarManager() {
        return mToolbarManager;
    }

    @Override
    public void destroy() {
        mMenuOrKeyboardActionController.unregisterMenuOrKeyboardActionHandler(this);

        mActivity.getLayoutManagerSupplier().removeObserver(mLayoutManagerSupplierCallback);

        if (mOverlayPanelManager != null) {
            mOverlayPanelManager.removeObserver(mOverlayPanelManagerObserver);
        }

        if (mActivity.getOverviewModeBehaviorSupplier() != null) {
            mActivity.getOverviewModeBehaviorSupplier().removeObserver(
                    mOverviewModeBehaviorSupplierCallback);
        }
        if (mOverviewModeBehavior != null) {
            mOverviewModeBehavior.removeOverviewModeObserver(mOverviewModeObserver);
        }

        if (mAppMenuCoordinator != null) {
            mAppMenuCoordinator.unregisterAppMenuBlocker(this);
            mAppMenuCoordinator.unregisterAppMenuBlocker(mActivity);
            mAppMenuCoordinator.destroy();
        }

        if (mTabThemeColorProvider != null) {
            mTabThemeColorProvider.destroy();
            mTabThemeColorProvider = null;
        }

        if (mFindToolbarManager != null) mFindToolbarManager.removeObserver(mFindToolbarObserver);

        if (mVrModeObserver != null) VrModuleProvider.unregisterVrModeObserver(mVrModeObserver);

        if (mModalDialogManagerObserver != null && mActivity.getModalDialogManager() != null) {
            mActivity.getModalDialogManager().removeObserver(mModalDialogManagerObserver);
        }

        if (mBottomSheetController != null) mBottomSheetController.destroy();

        mActivity = null;
    }

    @Override
    public void onPreInflationStartup() {
        initializeBottomSheetController();
    }

    @Override
    public void onPostInflationStartup() {
        ViewGroup coordinator = mActivity.findViewById(R.id.coordinator);
        mScrimView = new ScrimView(mActivity,
                mActivity.getStatusBarColorController().getStatusBarScrimDelegate(), coordinator);

        mTabThemeColorProvider = new TabThemeColorProvider(mActivity);
        mTabThemeColorProvider.setActivityTabProvider(mActivity.getActivityTabProvider());

        initializeToolbar();
        initAppMenu();
        initFindToolbarManager();
        initDirectActionInitializer();
        if (mAppMenuCoordinator != null) {
            mToolbarManager.onAppMenuInitialized(mAppMenuCoordinator);
            mModalDialogManagerObserver = new ModalDialogManagerObserver() {
                @Override
                public void onDialogShown(PropertyModel model) {
                    mAppMenuCoordinator.getAppMenuHandler().hideAppMenu();
                }

                @Override
                public void onDialogHidden(PropertyModel model) {}
            };
            mActivity.getModalDialogManager().addObserver(mModalDialogManagerObserver);
        }

        mVrModeObserver = new VrModeObserver() {
            @Override
            public void onEnterVr() {
                mFindToolbarManager.hideToolbar();
            }

            @Override
            public void onExitVr() {}
        };
        VrModuleProvider.registerVrModeObserver(mVrModeObserver);
    }

    /**
     * Triggered when the share menu item is selected.
     * This creates and shows a share intent picker dialog or starts a share intent directly.
     * @param shareDirectly Whether it should share directly with the activity that was most
     *                      recently used to share.
     * @param isIncognito Whether currentTab is incognito.
     */
    @VisibleForTesting
    public void onShareMenuItemSelected(final boolean shareDirectly, final boolean isIncognito) {
        if (mShareDelegateSupplier.get() == null) return;

        mShareDelegateSupplier.get().share(mActivityTabProvider.get(), shareDirectly);
    }

    // MenuOrKeyboardActionHandler implementation

    @Override
    public boolean handleMenuOrKeyboardAction(int id, boolean fromMenu) {
        if (id == R.id.show_menu && mAppMenuCoordinator != null) {
            mAppMenuCoordinator.showAppMenuForKeyboardEvent();
            return true;
        } else if (id == R.id.find_in_page_id) {
            if (mFindToolbarManager == null) return false;

            mFindToolbarManager.showToolbar();

            Tab tab = mActivity.getActivityTabProvider().get();
            if (fromMenu) {
                RecordUserAction.record("MobileMenuFindInPage");
                new UkmRecorder.Bridge().recordEvent(tab.getWebContents(), "MobileMenu.FindInPage");
            } else {
                RecordUserAction.record("MobileShortcutFindInPage");
            }
            return true;
        } else if (id == R.id.share_menu_id || id == R.id.direct_share_menu_id) {
            onShareMenuItemSelected(id == R.id.direct_share_menu_id,
                    mActivity.getTabModelSelector().isIncognitoSelected());
        } else if (id == R.id.paint_preview_capture_id) {
            PaintPreviewUtils.capturePaintPreview(mActivity.getCurrentWebContents());
        }

        return false;
    }

    // AppMenuBlocker implementation

    @Override
    public boolean canShowAppMenu() {
        // TODO(https:crbug.com/931496): Eventually the ContextualSearchManager, EphemeralTabPanel,
        // and FindToolbarManager will all be owned by this class.

        // Do not show the menu if Contextual Search panel is opened.
        if (mActivity.getContextualSearchManager() != null
                && mActivity.getContextualSearchManager().isSearchPanelOpened()) {
            return false;
        }

        if (mActivity.getEphemeralTabPanel() != null
                && mActivity.getEphemeralTabPanel().isPanelOpened()) {
            return false;
        }

        // Do not show the menu if we are in find in page view.
        if (mFindToolbarManager != null && mFindToolbarManager.isShowing()
                && !DeviceFormFactor.isNonMultiDisplayContextOnTablet(mActivity)) {
            return false;
        }

        return true;
    }

    /**
     * Performs a direct action.
     *
     * @param actionId Name of the direct action to perform.
     * @param arguments Arguments for this action.
     * @param cancellationSignal Signal used to cancel a direct action from the caller.
     * @param callback Callback to run when the action is done.
     */
    public void onPerformDirectAction(String actionId, Bundle arguments,
            CancellationSignal cancellationSignal, Consumer<Bundle> callback) {
        if (mDirectActionInitializer == null) return;
        mDirectActionInitializer.onPerformDirectAction(
                actionId, arguments, cancellationSignal, callback);
    }

    /**
     * Lists direct actions supported.
     *
     * Returns a list of direct actions supported by the Activity associated with this
     * RootUiCoordinator.
     *
     * @param cancellationSignal Signal used to cancel a direct action from the caller.
     * @param callback Callback to run when the action is done.
     */
    public void onGetDirectActions(CancellationSignal cancellationSignal, Consumer callback) {
        if (mDirectActionInitializer == null) return;
        mDirectActionInitializer.onGetDirectActions(cancellationSignal, callback);
    }

    // Protected class methods

    protected void onLayoutManagerAvailable(LayoutManager layoutManager) {
        if (mOverlayPanelManager != null) {
            mOverlayPanelManager.removeObserver(mOverlayPanelManagerObserver);
        }
        mOverlayPanelManager = layoutManager.getOverlayPanelManager();

        if (mOverlayPanelManagerObserver == null) {
            mOverlayPanelManagerObserver = new OverlayPanelManager.OverlayPanelManagerObserver() {
                @Override
                public void onOverlayPanelShown() {
                    if (mFindToolbarManager != null) {
                        mFindToolbarManager.hideToolbar(false);
                    }
                }

                @Override
                public void onOverlayPanelHidden() {}
            };
        }

        mOverlayPanelManager.addObserver(mOverlayPanelManagerObserver);
    }

    /**
     * Constructs {@link ToolbarManager} and the handler necessary for controlling the menu on the
     * {@link Toolbar}.
     */
    protected void initializeToolbar() {
        try (TraceEvent te = TraceEvent.scoped("RootUiCoordinator.initializeToolbar")) {
            final View controlContainer = mActivity.findViewById(R.id.control_container);
            assert controlContainer != null;
            ToolbarControlContainer toolbarContainer = (ToolbarControlContainer) controlContainer;
            Callback<Boolean> urlFocusChangedCallback = hasFocus -> {
                if (mOnOmniboxFocusChangedListener != null) {
                    mOnOmniboxFocusChangedListener.onResult(hasFocus);
                }
            };
            mToolbarManager = new ToolbarManager(mActivity, toolbarContainer,
                    mActivity.getCompositorViewHolder().getInvalidator(), urlFocusChangedCallback,
                    mTabThemeColorProvider, mShareDelegateSupplier);
            if (!mActivity.supportsAppMenu()) {
                mToolbarManager.getToolbar().disableMenuButton();
            }
        }
    }

    // Private class methods

    private void initOverviewModeSupplierObserver() {
        if (mActivity.getOverviewModeBehaviorSupplier() != null) {
            mOverviewModeBehaviorSupplierCallback = overviewModeBehavior -> {
                if (mOverviewModeBehavior != null) {
                    mOverviewModeBehavior.removeOverviewModeObserver(mOverviewModeObserver);
                }

                mOverviewModeBehavior = overviewModeBehavior;

                if (mOverviewModeObserver == null) {
                    mOverviewModeObserver = new EmptyOverviewModeObserver() {
                        @Override
                        public void onOverviewModeStartedShowing(boolean showToolbar) {
                            if (mFindToolbarManager != null) mFindToolbarManager.hideToolbar();
                            hideAppMenu();
                        }

                        @Override
                        public void onOverviewModeFinishedShowing() {
                            // Ideally we wouldn't allow the app menu to show while animating the
                            // overview mode. This is hard to track, however, because in some
                            // instances #onOverviewModeStartedShowing is called after
                            // #onOverviewModeFinishedShowing (see https://crbug.com/969047).
                            // Once that bug is fixed, we can remove this call to hide in favor of
                            // disallowing app menu shows during animation. Alternatively, we
                            // could expose a way to query whether an animation is in progress.
                            hideAppMenu();
                        }

                        @Override
                        public void onOverviewModeStartedHiding(
                                boolean showToolbar, boolean delayAnimation) {
                            hideAppMenu();
                        }

                        @Override
                        public void onOverviewModeFinishedHiding() {
                            hideAppMenu();
                        }
                    };
                }
                mOverviewModeBehavior.addOverviewModeObserver(mOverviewModeObserver);
            };
            mActivity.getOverviewModeBehaviorSupplier().addObserver(
                    mOverviewModeBehaviorSupplierCallback);
        }
    }

    private void initAppMenu() {
        // TODO(https://crbug.com/931496): Revisit this as part of the broader
        // discussion around activity-specific UI customizations.
        if (mActivity.supportsAppMenu()) {
            mAppMenuCoordinator = AppMenuCoordinatorFactory.createAppMenuCoordinator(mActivity,
                    mActivity.getLifecycleDispatcher(), mToolbarManager, mActivity,
                    mActivity.getWindow().getDecorView(),
                    mActivity.getWindow().getDecorView().findViewById(R.id.menu_anchor_stub));

            mAppMenuCoordinator.registerAppMenuBlocker(this);
            mAppMenuCoordinator.registerAppMenuBlocker(mActivity);
        }
    }

    private void hideAppMenu() {
        if (mAppMenuCoordinator != null) mAppMenuCoordinator.getAppMenuHandler().hideAppMenu();
    }

    private void initFindToolbarManager() {
        if (!mActivity.supportsFindInPage()) return;

        int stubId = R.id.find_toolbar_stub;
        if (DeviceFormFactor.isNonMultiDisplayContextOnTablet(mActivity)) {
            stubId = R.id.find_toolbar_tablet_stub;
        }
        mFindToolbarManager = new FindToolbarManager(mActivity.findViewById(stubId),
                mActivity.getTabModelSelector(), mActivity.getWindowAndroid(),
                mToolbarManager.getActionModeControllerCallback());

        mFindToolbarObserver = new FindToolbarObserver() {
            @Override
            public void onFindToolbarShown() {
                if (mActivity.getContextualSearchManager() != null) {
                    mActivity.getContextualSearchManager().hideContextualSearch(
                            OverlayPanel.StateChangeReason.UNKNOWN);
                }
                if (mActivity.getEphemeralTabPanel() != null) {
                    mActivity.getEphemeralTabPanel().closePanel(
                            OverlayPanel.StateChangeReason.UNKNOWN, true);
                }
            }

            @Override
            public void onFindToolbarHidden() {}
        };

        mFindToolbarManager.addObserver(mFindToolbarObserver);

        mActivity.getToolbarManager().setFindToolbarManager(mFindToolbarManager);
    }

    /**
     * Initialize the {@link BottomSheetController}. The view for this component is not created
     * until content is requested in the sheet.
     */
    private void initializeBottomSheetController() {
        Supplier<View> sheetViewSupplier = () -> {
            ViewGroup coordinator = mActivity.findViewById(R.id.coordinator);
            mActivity.getLayoutInflater().inflate(R.layout.bottom_sheet, coordinator);

            View sheet = coordinator.findViewById(R.id.bottom_sheet);

            mBottomSheetSnackbarManager = new SnackbarManager(mActivity,
                    sheet.findViewById(R.id.bottom_sheet_snackbar_container),
                    mActivity.getWindowAndroid());

            return sheet;
        };

        Supplier<OverlayPanelManager> panelManagerSupplier = ()
                -> mActivity.getCompositorViewHolder().getLayoutManager().getOverlayPanelManager();

        mBottomSheetController = new BottomSheetController(mActivity.getLifecycleDispatcher(),
                mActivityTabProvider, this::getScrim, sheetViewSupplier, panelManagerSupplier,
                mActivity.getFullscreenManager(), mActivity.getWindow(),
                mActivity.getWindowAndroid().getKeyboardDelegate());

        mBottomSheetManager = new BottomSheetManager(mBottomSheetController, mActivityTabProvider,
                mActivity::getFullscreenManager, mActivity::getModalDialogManager,
                this::getBottomSheetSnackbarManager, mActivity);
    }

    /** @return The {@link BottomSheetController} for this activity. */
    public BottomSheetController getBottomSheetController() {
        return mBottomSheetController;
    }

    /** @return The root coordinator / activity's primary scrim. */
    public ScrimView getScrim() {
        return mScrimView;
    }

    /** @return The {@link SnackbarManager} for the {@link BottomSheetController}. */
    public SnackbarManager getBottomSheetSnackbarManager() {
        return mBottomSheetSnackbarManager;
    }

    private void initDirectActionInitializer() {
        @ActivityType
        int activityType = mActivity.getActivityType();
        TabModelSelector tabModelSelector = mActivity.getTabModelSelector();
        mDirectActionInitializer = new DirectActionInitializer(mActivity, activityType, mActivity,
                mActivity::onBackPressed, tabModelSelector, mFindToolbarManager,
                mActivity.getBottomSheetController(), mScrimView);
        mActivity.getLifecycleDispatcher().register(mDirectActionInitializer);
    }

    // Testing methods

    @VisibleForTesting
    public AppMenuCoordinator getAppMenuCoordinatorForTesting() {
        return mAppMenuCoordinator;
    }
}
