// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <fuchsia/accessibility/semantics/cpp/fidl_test_base.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>
#include <zircon/types.h>

#include "base/auto_reset.h"
#include "base/fuchsia/default_context.h"
#include "base/fuchsia/scoped_service_binding.h"
#include "base/logging.h"
#include "base/test/bind_test_util.h"
#include "content/public/browser/web_contents_observer.h"
#include "fuchsia/base/frame_test_util.h"
#include "fuchsia/base/test_navigation_listener.h"
#include "fuchsia/engine/browser/accessibility_bridge.h"
#include "fuchsia/engine/browser/frame_impl.h"
#include "fuchsia/engine/test/test_data.h"
#include "fuchsia/engine/test/web_engine_browser_test.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/switches.h"
#include "ui/ozone/public/ozone_switches.h"

using fuchsia::accessibility::semantics::Node;
using fuchsia::accessibility::semantics::SemanticListener;
using fuchsia::accessibility::semantics::SemanticsManager;
using fuchsia::accessibility::semantics::SemanticTree;

namespace {

const char kPage1Path[] = "/ax1.html";
const char kPage2Path[] = "/batching.html";
const char kPage1Title[] = "accessibility 1";
const char kPage2Title[] = "lots of nodes!";
const char kButtonName[] = "a button";
const char kNodeName[] = "last node";
const char kParagraphName[] = "a third paragraph";
const size_t kPage1NodeCount = 9;
const size_t kPage2NodeCount = 190;

class FakeSemanticTree
    : public fuchsia::accessibility::semantics::testing::SemanticTree_TestBase {
 public:
  FakeSemanticTree() = default;
  ~FakeSemanticTree() override = default;

  // fuchsia::accessibility::semantics::SemanticTree implementation.
  void UpdateSemanticNodes(std::vector<Node> nodes) final {
    for (auto& node : nodes)
      nodes_.push_back(std::move(node));
  }

  void DeleteSemanticNodes(std::vector<uint32_t> node_ids) final {
    for (auto id : node_ids) {
      for (uint i = 0; i < nodes_.size(); i++) {
        if (nodes_.at(i).node_id() == id)
          nodes_.erase(nodes_.begin() + i);
      }
    }
  }

  void CommitUpdates(CommitUpdatesCallback callback) final {
    callback();
    if (on_commit_updates_)
      on_commit_updates_.Run();
  }

  void NotImplemented_(const std::string& name) final {
    NOTIMPLEMENTED() << name;
  }

  void RunUntilNodeCountAtLeast(size_t count) {
    DCHECK(!on_commit_updates_);
    if (nodes_.size() >= count)
      return;

    base::RunLoop run_loop;
    base::AutoReset<base::RepeatingClosure> auto_reset(
        &on_commit_updates_,
        base::BindLambdaForTesting([this, count, &run_loop]() {
          if (nodes_.size() >= count) {
            run_loop.Quit();
          }
        }));
    run_loop.Run();
  }

  bool HasNodeWithLabel(base::StringPiece name) {
    for (auto& node : nodes_) {
      if (node.has_attributes() && node.attributes().has_label() &&
          node.attributes().label() == name) {
        return true;
      }
    }
    return false;
  }

  Node* GetNodeFromLabel(base::StringPiece name) {
    for (auto& node : nodes_) {
      if (node.has_attributes() && node.attributes().has_label() &&
          node.attributes().label() == name) {
        return &node;
      }
    }
    return nullptr;
  }

 private:
  std::vector<Node> nodes_;
  base::RepeatingClosure on_commit_updates_;

  DISALLOW_COPY_AND_ASSIGN(FakeSemanticTree);
};

class FakeSemanticsManager : public fuchsia::accessibility::semantics::testing::
                                 SemanticsManager_TestBase {
 public:
  FakeSemanticsManager() : semantic_tree_binding_(&semantic_tree_) {}
  ~FakeSemanticsManager() override = default;

  bool is_view_registered() const { return view_ref_.reference.is_valid(); }
  bool is_listener_valid() const { return static_cast<bool>(listener_); }
  FakeSemanticTree* semantic_tree() { return &semantic_tree_; }

  // Directly call the listener to simulate Fuchsia setting the semantics mode.
  void SetSemanticsModeEnabled(bool is_enabled) {
    listener_->OnSemanticsModeChanged(is_enabled, []() {});
  }

  // Pumps the message loop until the RegisterViewForSemantics() is called.
  void WaitUntilViewRegistered() {
    base::RunLoop loop;
    on_view_registered_ = loop.QuitClosure();
    loop.Run();
  }

  // The value returned by hit testing is written to a class member. In the case
  // Run() times out, the function continues so we don't want to write to a
  // local variable.
  uint32_t HitTestAtPointSync(fuchsia::math::PointF target_point) {
    hit_test_result_.reset();
    base::RunLoop run_loop;
    listener_->HitTest(target_point,
                       [quit = run_loop.QuitClosure(),
                        this](fuchsia::accessibility::semantics::Hit hit) {
                         if (hit.has_node_id()) {
                           hit_test_result_ = hit.node_id();
                         }
                         quit.Run();
                       });
    run_loop.Run();

    return hit_test_result_.value();
  }

  // fuchsia::accessibility::semantics::SemanticsManager implementation.
  void RegisterViewForSemantics(
      fuchsia::ui::views::ViewRef view_ref,
      fidl::InterfaceHandle<SemanticListener> listener,
      fidl::InterfaceRequest<SemanticTree> semantic_tree_request) final {
    view_ref_ = std::move(view_ref);
    listener_ = listener.Bind();
    semantic_tree_binding_.Bind(std::move(semantic_tree_request));
    std::move(on_view_registered_).Run();
  }

  void NotImplemented_(const std::string& name) final {
    NOTIMPLEMENTED() << name;
  }

 private:
  fuchsia::ui::views::ViewRef view_ref_;
  fuchsia::accessibility::semantics::SemanticListenerPtr listener_;
  FakeSemanticTree semantic_tree_;
  fidl::Binding<SemanticTree> semantic_tree_binding_;
  base::Optional<uint32_t> hit_test_result_;
  base::OnceClosure on_view_registered_;

  DISALLOW_COPY_AND_ASSIGN(FakeSemanticsManager);
};

fuchsia::math::PointF GetCenterOfBox(fuchsia::ui::gfx::BoundingBox box) {
  fuchsia::math::PointF center;
  center.x = (box.min.x + box.max.x) / 2;
  center.y = (box.min.y + box.max.y) / 2;
  return center;
}

}  // namespace

class AccessibilityBridgeTest : public cr_fuchsia::WebEngineBrowserTest {
 public:
  AccessibilityBridgeTest() : semantics_manager_binding_(&semantics_manager_) {
    cr_fuchsia::WebEngineBrowserTest::set_test_server_root(
        base::FilePath(cr_fuchsia::kTestServerRoot));
  }

  ~AccessibilityBridgeTest() override = default;

  void SetUp() override {
    base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
    command_line->AppendSwitchNative(switches::kOzonePlatform,
                                     switches::kHeadless);
    command_line->AppendSwitch(switches::kHeadless);
    cr_fuchsia::WebEngineBrowserTest::SetUp();
  }

  void SetUpOnMainThread() override {
    fuchsia::accessibility::semantics::SemanticsManagerPtr
        semantics_manager_ptr;
    semantics_manager_binding_.Bind(semantics_manager_ptr.NewRequest());

    frame_ptr_ =
        cr_fuchsia::WebEngineBrowserTest::CreateFrame(&navigation_listener_);
    frame_impl_ = context_impl()->GetFrameImplForTest(&frame_ptr_);
    frame_impl_->set_semantics_manager_for_test(
        std::move(semantics_manager_ptr));
    frame_ptr_->EnableHeadlessRendering();

    semantics_manager_.WaitUntilViewRegistered();
    ASSERT_TRUE(semantics_manager_.is_view_registered());
    ASSERT_TRUE(semantics_manager_.is_listener_valid());
  }

 protected:
  fuchsia::web::FramePtr frame_ptr_;
  FrameImpl* frame_impl_;
  FakeSemanticsManager semantics_manager_;
  fidl::Binding<SemanticsManager> semantics_manager_binding_;
  cr_fuchsia::TestNavigationListener navigation_listener_;

  DISALLOW_COPY_AND_ASSIGN(AccessibilityBridgeTest);
};

// Test registration to the SemanticsManager and accessibility mode on
// WebContents is set correctly.
IN_PROC_BROWSER_TEST_F(AccessibilityBridgeTest, RegisterViewRef) {
  // Change the accessibility mode on the Fuchsia side and check that it is
  // propagated correctly.
  ASSERT_FALSE(frame_impl_->web_contents_for_test()
                   ->IsWebContentsOnlyAccessibilityModeForTesting());
  semantics_manager_.SetSemanticsModeEnabled(true);

  // Spin the loop to let the FrameImpl receive the mode-change.
  base::RunLoop().RunUntilIdle();

  EXPECT_TRUE(frame_impl_->web_contents_for_test()
                  ->IsWebContentsOnlyAccessibilityModeForTesting());
}

IN_PROC_BROWSER_TEST_F(AccessibilityBridgeTest, CorrectDataSent) {
  fuchsia::web::NavigationControllerPtr controller;
  frame_ptr_->GetNavigationController(controller.NewRequest());
  ASSERT_TRUE(embedded_test_server()->Start());
  semantics_manager_.SetSemanticsModeEnabled(true);

  GURL page_url1(embedded_test_server()->GetURL(kPage1Path));
  ASSERT_TRUE(cr_fuchsia::LoadUrlAndExpectResponse(
      controller.get(), fuchsia::web::LoadUrlParams(), page_url1.spec()));
  navigation_listener_.RunUntilUrlAndTitleEquals(page_url1, kPage1Title);

  // Check that the data values are correct in the FakeSemanticTree.
  // TODO(fxb/18796): Test more fields once Chrome to Fuchsia conversions are
  // available.
  semantics_manager_.semantic_tree()->RunUntilNodeCountAtLeast(kPage1NodeCount);
  EXPECT_TRUE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kPage1Title));
  EXPECT_TRUE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kButtonName));
  EXPECT_TRUE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kParagraphName));
}

// Batching is performed when the number of nodes to send or delete exceeds the
// maximum, as set on the Fuchsia side. Check that all nodes are received by the
// Semantic Tree when batching is performed.
IN_PROC_BROWSER_TEST_F(AccessibilityBridgeTest, DataSentWithBatching) {
  fuchsia::web::NavigationControllerPtr controller;
  frame_ptr_->GetNavigationController(controller.NewRequest());
  ASSERT_TRUE(embedded_test_server()->Start());
  semantics_manager_.SetSemanticsModeEnabled(true);

  GURL page_url2(embedded_test_server()->GetURL(kPage2Path));
  ASSERT_TRUE(cr_fuchsia::LoadUrlAndExpectResponse(
      controller.get(), fuchsia::web::LoadUrlParams(), page_url2.spec()));
  navigation_listener_.RunUntilUrlAndTitleEquals(page_url2, kPage2Title);

  // Run until we expect more than a batch's worth of nodes to be present.
  semantics_manager_.semantic_tree()->RunUntilNodeCountAtLeast(kPage2NodeCount);
  EXPECT_TRUE(semantics_manager_.semantic_tree()->HasNodeWithLabel(kNodeName));
}

// Check that semantics information is correctly sent when navigating from page
// to page.
IN_PROC_BROWSER_TEST_F(AccessibilityBridgeTest, TestNavigation) {
  fuchsia::web::NavigationControllerPtr controller;
  frame_ptr_->GetNavigationController(controller.NewRequest());
  ASSERT_TRUE(embedded_test_server()->Start());
  semantics_manager_.SetSemanticsModeEnabled(true);

  GURL page_url1(embedded_test_server()->GetURL(kPage1Path));
  ASSERT_TRUE(cr_fuchsia::LoadUrlAndExpectResponse(
      controller.get(), fuchsia::web::LoadUrlParams(), page_url1.spec()));
  navigation_listener_.RunUntilUrlAndTitleEquals(page_url1, kPage1Title);

  semantics_manager_.semantic_tree()->RunUntilNodeCountAtLeast(kPage1NodeCount);
  EXPECT_TRUE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kPage1Title));
  EXPECT_TRUE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kButtonName));
  EXPECT_TRUE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kParagraphName));

  GURL page_url2(embedded_test_server()->GetURL(kPage2Path));
  ASSERT_TRUE(cr_fuchsia::LoadUrlAndExpectResponse(
      controller.get(), fuchsia::web::LoadUrlParams(), page_url2.spec()));

  semantics_manager_.semantic_tree()->RunUntilNodeCountAtLeast(kPage2NodeCount);
  EXPECT_TRUE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kPage2Title));
  EXPECT_TRUE(semantics_manager_.semantic_tree()->HasNodeWithLabel(kNodeName));

  // Check that data from the first page has been deleted successfully.
  EXPECT_FALSE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kButtonName));
  EXPECT_FALSE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kParagraphName));
}

// Checks that the correct node ID is returned when performing hit testing.
IN_PROC_BROWSER_TEST_F(AccessibilityBridgeTest, HitTest) {
  fuchsia::web::NavigationControllerPtr controller;
  frame_ptr_->GetNavigationController(controller.NewRequest());
  ASSERT_TRUE(embedded_test_server()->Start());
  semantics_manager_.SetSemanticsModeEnabled(true);

  GURL page_url1(embedded_test_server()->GetURL(kPage1Path));
  ASSERT_TRUE(cr_fuchsia::LoadUrlAndExpectResponse(
      controller.get(), fuchsia::web::LoadUrlParams(), page_url1.spec()));
  navigation_listener_.RunUntilUrlAndTitleEquals(page_url1, kPage1Title);
  semantics_manager_.semantic_tree()->RunUntilNodeCountAtLeast(kPage1NodeCount);
  EXPECT_TRUE(
      semantics_manager_.semantic_tree()->HasNodeWithLabel(kParagraphName));

  Node* hit_test_node =
      semantics_manager_.semantic_tree()->GetNodeFromLabel(kParagraphName);

  fuchsia::math::PointF target_point =
      GetCenterOfBox(hit_test_node->location());

  EXPECT_EQ(hit_test_node->node_id(),
            semantics_manager_.HitTestAtPointSync(std::move(target_point)));

  // Expect hit testing to return the root when the point given is out of
  // bounds or there is no semantic node at that position.
  target_point.x = -1;
  target_point.y = -1;
  EXPECT_EQ(0u, semantics_manager_.HitTestAtPointSync(std::move(target_point)));
  target_point.x = 1;
  target_point.y = 1;
  EXPECT_EQ(0u, semantics_manager_.HitTestAtPointSync(std::move(target_point)));
}
