// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <objbase.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/macros.h"
#include "base/process/process_handle.h"
#include "base/strings/pattern.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/win/scoped_bstr.h"
#include "base/win/scoped_variant.h"
#include "build/build_config.h"
#include "content/browser/accessibility/accessibility_browsertest.h"
#include "content/browser/accessibility/accessibility_event_recorder.h"
#include "content/browser/accessibility/accessibility_tree_formatter_utils_win.h"
#include "content/browser/accessibility/browser_accessibility_manager_win.h"
#include "content/browser/accessibility/browser_accessibility_win.h"
#include "content/browser/renderer_host/render_view_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_view_aura.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/browser/web_contents/web_contents_view_aura.h"
#include "content/public/browser/accessibility_tree_formatter.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "content/public/test/accessibility_notification_waiter.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/shell/browser/shell.h"
#include "content/test/content_browser_test_utils_internal.h"
#include "net/base/escape.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "third_party/iaccessible2/ia2_api_all.h"
#include "third_party/isimpledom/ISimpleDOMNode.h"
#include "ui/accessibility/accessibility_switches.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"

namespace content {

namespace {

// AccessibilityWinBrowserTest ------------------------------------------------

class AccessibilityWinBrowserTest : public AccessibilityBrowserTest {
 public:
  AccessibilityWinBrowserTest();
  ~AccessibilityWinBrowserTest() override;

 protected:
  class AccessibleChecker;
  base::string16 PrintAXTree() const;
  void SetUpInputField(Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpScrollableInputField(
      Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpScrollableInputTypeSearchField(
      Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpSingleCharInputField(
      Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpSingleCharInputFieldWithPlaceholder(
      Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpSingleCharTextarea(
      Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpSingleCharContenteditable(
      Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpSingleCharRtlInputField(
      Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpTextareaField(
      Microsoft::WRL::ComPtr<IAccessibleText>* textarea_text);
  void SetUpSampleParagraph(
      Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text,
      ui::AXMode accessibility_mode = ui::kAXModeComplete);
  void SetUpSampleParagraphInScrollableDocument(
      Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text,
      ui::AXMode accessibility_mode = ui::kAXModeComplete);
  void SetUpVeryTallPadding(
      Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text,
      ui::AXMode accessibility_mode = ui::kAXModeComplete);
  void SetUpVeryTallMargin(
      Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text,
      ui::AXMode accessibility_mode = ui::kAXModeComplete);
  void SetUpSampleParagraphInScrollableEditable(
      Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text);
  BrowserAccessibility* FindNode(ax::mojom::Role role,
                                 const std::string& name_or_value);
  BrowserAccessibilityManager* GetManager();
  static Microsoft::WRL::ComPtr<IAccessible> GetAccessibleFromVariant(
      IAccessible* parent,
      VARIANT* var);
  static HRESULT QueryIAccessible2(IAccessible* accessible,
                                   IAccessible2** accessible2);
  static void FindNodeInAccessibilityTree(IAccessible* node,
                                          int32_t expected_role,
                                          const std::wstring& expected_name,
                                          int32_t depth,
                                          bool* found);
  static void CheckTextAtOffset(Microsoft::WRL::ComPtr<IAccessibleText>& object,
                                LONG offset,
                                IA2TextBoundaryType boundary_type,
                                LONG expected_start_offset,
                                LONG expected_end_offset,
                                const std::wstring& expected_text);
  static std::vector<base::win::ScopedVariant> GetAllAccessibleChildren(
      IAccessible* element);

 private:
  void SetUpInputFieldHelper(
      Microsoft::WRL::ComPtr<IAccessibleText>* input_text);
  void SetUpSampleParagraphHelper(
      Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text);
  BrowserAccessibility* FindNodeInSubtree(BrowserAccessibility& node,
                                          ax::mojom::Role role,
                                          const std::string& name_or_value);
  DISALLOW_COPY_AND_ASSIGN(AccessibilityWinBrowserTest);
};

AccessibilityWinBrowserTest::AccessibilityWinBrowserTest() = default;

AccessibilityWinBrowserTest::~AccessibilityWinBrowserTest() = default;

base::string16 AccessibilityWinBrowserTest::PrintAXTree() const {
  std::unique_ptr<AccessibilityTreeFormatter> formatter(
      AccessibilityTreeFormatter::Create());
  DCHECK(formatter);
  formatter->set_show_ids(true);
  formatter->SetPropertyFilters({AccessibilityTreeFormatter::PropertyFilter(
      L"*", AccessibilityTreeFormatter::PropertyFilter::ALLOW)});

  base::string16 str;
  formatter->FormatAccessibilityTreeForTesting(
      GetRootAccessibilityNode(shell()->web_contents()), &str);
  return str;
}

// Loads a page with  an input text field and places sample text in it. Also,
// places the caret before the last character.
void AccessibilityWinBrowserTest::SetUpInputField(
    Microsoft::WRL::ComPtr<IAccessibleText>* input_text) {
  ASSERT_NE(nullptr, input_text);
  LoadInputField();
  SetUpInputFieldHelper(input_text);
}

// Loads a page with  an input text field and places sample text in it that
// overflows its width. Also, places the caret before the last character.
void AccessibilityWinBrowserTest::SetUpScrollableInputField(
    Microsoft::WRL::ComPtr<IAccessibleText>* input_text) {
  ASSERT_NE(nullptr, input_text);
  LoadInitialAccessibilityTreeFromHtml(
      std::string(
          R"HTML(<!DOCTYPE html>
          <html>
          <body>
            <input type="text" style="width: 150px;" value=")HTML") +
      net::EscapeForHTML(InputContentsString()) + std::string(R"HTML(">
          </body>
          </html>)HTML"));

  SetUpInputFieldHelper(input_text);
}

// Loads a page with  an input text field and places sample text in it that
// overflows its width. Also, places the caret before the last character.
void AccessibilityWinBrowserTest::SetUpScrollableInputTypeSearchField(
    Microsoft::WRL::ComPtr<IAccessibleText>* input_text) {
  ASSERT_NE(nullptr, input_text);
  LoadInitialAccessibilityTreeFromHtml(
      std::string(
          R"HTML(<!DOCTYPE html>
          <html>
          <body>
            <input type="search" style="width: 150px;" value=")HTML") +
      net::EscapeForHTML(InputContentsString()) + std::string(R"HTML(">
          </body>
          </html>)HTML"));

  SetUpInputFieldHelper(input_text);
}

// Loads a page with an input text field and places a single character in it.
// Also tests with padding, in order to ensure character extent of empty field
// does not erroneously include padding.
void AccessibilityWinBrowserTest::SetUpSingleCharInputField(
    Microsoft::WRL::ComPtr<IAccessibleText>* input_text) {
  ASSERT_NE(nullptr, input_text);
  LoadInitialAccessibilityTreeFromHtml(std::string(
      R"HTML(<!DOCTYPE html>
          <html>
          <body>
            <form>
              <input type="text" value="x" style="padding:3px">
            </form>
          </body>
          </html>)HTML"));
  SetUpInputFieldHelper(input_text);
}

void AccessibilityWinBrowserTest::SetUpSingleCharInputFieldWithPlaceholder(
    Microsoft::WRL::ComPtr<IAccessibleText>* input_text) {
  ASSERT_NE(nullptr, input_text);
  LoadInitialAccessibilityTreeFromHtml(std::string(
      R"HTML(<!DOCTYPE html>
          <html>
          <body>
            <form>
              <input type="text" value="x" placeholder="placeholder">
            </form>
          </body>
          </html>)HTML"));
  SetUpInputFieldHelper(input_text);
}

void AccessibilityWinBrowserTest::SetUpSingleCharTextarea(
    Microsoft::WRL::ComPtr<IAccessibleText>* input_text) {
  ASSERT_NE(nullptr, input_text);
  LoadInitialAccessibilityTreeFromHtml(std::string(
      R"HTML(<!DOCTYPE html>
          <html>
          <body>
            <form>
              <textarea rows="3" cols="10">x</textarea>
            </form>
          </body>
          </html>)HTML"));
  SetUpInputFieldHelper(input_text);
}

// Loads a page with a right-to-left input text field and places a single
// character in it.
void AccessibilityWinBrowserTest::SetUpSingleCharRtlInputField(
    Microsoft::WRL::ComPtr<IAccessibleText>* input_text) {
  ASSERT_NE(nullptr, input_text);
  LoadInitialAccessibilityTreeFromHtml(std::string(
      R"HTML(<!DOCTYPE html>
          <html>
          <body>
            <form>
              <input type="text" id="textField" name="name" dir="rtl" value="x">
            </form>
          </body>
          </html>)HTML"));
  SetUpInputFieldHelper(input_text);
}

void AccessibilityWinBrowserTest::SetUpInputFieldHelper(
    Microsoft::WRL::ComPtr<IAccessibleText>* input_text) {
  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> div;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      div.GetAddressOf()));
  std::vector<base::win::ScopedVariant> div_children =
      GetAllAccessibleChildren(div.Get());
  ASSERT_LT(0u, div_children.size());

  // The input field is always the last child.
  Microsoft::WRL::ComPtr<IAccessible2> input;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(div.Get(),
                               div_children[div_children.size() - 1].AsInput())
          .Get(),
      input.GetAddressOf()));
  LONG input_role = 0;
  ASSERT_HRESULT_SUCCEEDED(input->role(&input_role));
  ASSERT_EQ(ROLE_SYSTEM_TEXT, input_role);

  // Retrieve the IAccessibleText interface for the field.
  ASSERT_HRESULT_SUCCEEDED(input.CopyTo(input_text->GetAddressOf()));

  // Set the caret before the last character.
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  std::wstring caret_offset = base::UTF16ToWide(
      base::NumberToString16(InputContentsString().size() - 1));
  ExecuteScript(
      std::wstring(L"let textField = document.querySelector('input,textarea');"
                   L"textField.focus();"
                   L"textField.setSelectionRange(") +
      caret_offset + L"," + caret_offset +
      L");"
      L"textField.scrollLeft = 1000;");
  waiter.WaitForNotification();
}

// Loads a page with  a textarea text field and places sample text in it. Also,
// places the caret before the last character.
void AccessibilityWinBrowserTest::SetUpTextareaField(
    Microsoft::WRL::ComPtr<IAccessibleText>* textarea_text) {
  ASSERT_NE(nullptr, textarea_text);
  LoadTextareaField();

  // Retrieve the IAccessible interface for the web page.
  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> section;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      section.GetAddressOf()));
  std::vector<base::win::ScopedVariant> section_children =
      GetAllAccessibleChildren(section.Get());
  ASSERT_EQ(1u, section_children.size());

  // Find the textarea text field.
  Microsoft::WRL::ComPtr<IAccessible2> textarea;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(section.Get(), section_children[0].AsInput())
          .Get(),
      textarea.GetAddressOf()));
  LONG textarea_role = 0;
  ASSERT_HRESULT_SUCCEEDED(textarea->role(&textarea_role));
  ASSERT_EQ(ROLE_SYSTEM_TEXT, textarea_role);

  // Retrieve the IAccessibleText interface for the field.
  ASSERT_HRESULT_SUCCEEDED(textarea.CopyTo(textarea_text->GetAddressOf()));

  // Set the caret before the last character.
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  std::wstring caret_offset = base::UTF16ToWide(
      base::NumberToString16(InputContentsString().size() - 1));
  ExecuteScript(
      std::wstring(L"var textField = document.querySelector('textarea');"
                   L"textField.focus();"
                   L"textField.setSelectionRange(") +
      caret_offset + L"," + caret_offset + L");");
  waiter.WaitForNotification();
}

// Loads a page with  a paragraph of sample text.
void AccessibilityWinBrowserTest::SetUpSampleParagraph(
    Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text,
    ui::AXMode accessibility_mode) {
  LoadSampleParagraph(accessibility_mode);
  SetUpSampleParagraphHelper(accessible_text);
}

// Loads a page with a paragraph of sample text which is below the
// bottom of the screen.
void AccessibilityWinBrowserTest::SetUpSampleParagraphInScrollableDocument(
    Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text,
    ui::AXMode accessibility_mode) {
  LoadSampleParagraphInScrollableDocument(accessibility_mode);
  SetUpSampleParagraphHelper(accessible_text);
}

// Loads a page with a paragraph of sample text which is below the
// bottom of the screen, where the bounds of the paragraph
// are longer than one screenful.
void AccessibilityWinBrowserTest::SetUpVeryTallPadding(
    Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text,
    ui::AXMode accessibility_mode) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
      <html>
      <body>
        <p style="padding-top:200vh; padding-bottom:200vh">ABC<br/>DEF</p>
      </body>
      </html>)HTML",
      accessibility_mode);

  SetUpSampleParagraphHelper(accessible_text);
}

// Loads a page with a paragraph of sample text which is below the
// bottom of the screen, where the bounds of the paragraph
// are longer than one screenful.
void AccessibilityWinBrowserTest::SetUpVeryTallMargin(
    Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text,
    ui::AXMode accessibility_mode) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
      <html>
      <body>
        <p style="margin-top:200vh; margin-bottom:200vh">ABC<br/>DEF</p>
      </body>
      </html>)HTML",
      accessibility_mode);

  SetUpSampleParagraphHelper(accessible_text);
}

void AccessibilityWinBrowserTest::SetUpSampleParagraphInScrollableEditable(
    Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text) {
  LoadSampleParagraphInScrollableEditable();
  SetUpSampleParagraphHelper(accessible_text);
}

void AccessibilityWinBrowserTest::SetUpSampleParagraphHelper(
    Microsoft::WRL::ComPtr<IAccessibleText>* accessible_text) {
  ASSERT_NE(nullptr, accessible_text);

  // Retrieve the IAccessible interface for the web page.
  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      paragraph.GetAddressOf()));

  LONG paragraph_role = 0;
  ASSERT_HRESULT_SUCCEEDED(paragraph->role(&paragraph_role));
  ASSERT_EQ(IA2_ROLE_PARAGRAPH, paragraph_role);

  ASSERT_HRESULT_SUCCEEDED(paragraph.CopyTo(accessible_text->GetAddressOf()));
}

// Retrieve the accessibility node, starting from the root node, that matches
// the accessibility role, name or value.
BrowserAccessibility* AccessibilityWinBrowserTest::FindNode(
    ax::mojom::Role role,
    const std::string& name_or_value) {
  BrowserAccessibility* root = GetManager()->GetRoot();
  CHECK(root);
  return FindNodeInSubtree(*root, role, name_or_value);
}

// Retrieve the browser accessibility manager object for the current web
// contents.
BrowserAccessibilityManager* AccessibilityWinBrowserTest::GetManager() {
  WebContentsImpl* web_contents =
      static_cast<WebContentsImpl*>(shell()->web_contents());
  return web_contents->GetRootBrowserAccessibilityManager();
}

// Retrieve the accessibility node in the subtree that matches the accessibility
// role, name or value.
BrowserAccessibility* AccessibilityWinBrowserTest::FindNodeInSubtree(
    BrowserAccessibility& node,
    ax::mojom::Role role,
    const std::string& name_or_value) {
  const auto& name = node.GetStringAttribute(ax::mojom::StringAttribute::kName);
  const auto& value =
      node.GetStringAttribute(ax::mojom::StringAttribute::kValue);
  if (node.GetRole() == role &&
      (name == name_or_value || value == name_or_value)) {
    return &node;
  }

  for (unsigned int i = 0; i < node.PlatformChildCount(); ++i) {
    BrowserAccessibility* result =
        FindNodeInSubtree(*node.PlatformGetChild(i), role, name_or_value);
    if (result)
      return result;
  }
  return nullptr;
}
// Static helpers ------------------------------------------------

Microsoft::WRL::ComPtr<IAccessible>
AccessibilityWinBrowserTest::GetAccessibleFromVariant(IAccessible* parent,
                                                      VARIANT* var) {
  Microsoft::WRL::ComPtr<IAccessible> ptr;
  switch (V_VT(var)) {
    case VT_DISPATCH: {
      IDispatch* dispatch = V_DISPATCH(var);
      if (dispatch)
        dispatch->QueryInterface(ptr.GetAddressOf());
      break;
    }

    case VT_I4: {
      Microsoft::WRL::ComPtr<IDispatch> dispatch;
      HRESULT hr = parent->get_accChild(*var, dispatch.GetAddressOf());
      EXPECT_TRUE(SUCCEEDED(hr));
      if (dispatch.Get())
        dispatch.CopyTo(ptr.GetAddressOf());
      break;
    }
  }
  return ptr;
}

HRESULT AccessibilityWinBrowserTest::QueryIAccessible2(
    IAccessible* accessible,
    IAccessible2** accessible2) {
  // IA2 Spec dictates that IServiceProvider should be used instead of
  // QueryInterface when retrieving IAccessible2.
  Microsoft::WRL::ComPtr<IServiceProvider> service_provider;
  HRESULT hr = accessible->QueryInterface(service_provider.GetAddressOf());
  return SUCCEEDED(hr)
             ? service_provider->QueryService(IID_IAccessible2, accessible2)
             : hr;
}

// Recursively search through all of the descendants reachable from an
// IAccessible node and return true if we find one with the given role
// and name.
void AccessibilityWinBrowserTest::FindNodeInAccessibilityTree(
    IAccessible* node,
    int32_t expected_role,
    const std::wstring& expected_name,
    int32_t depth,
    bool* found) {
  base::win::ScopedBstr name_bstr;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  node->get_accName(childid_self, name_bstr.Receive());
  std::wstring name(name_bstr, name_bstr.Length());
  base::win::ScopedVariant role;
  node->get_accRole(childid_self, role.Receive());
  ASSERT_EQ(VT_I4, role.type());

  // Print the accessibility tree as we go, because if this test fails
  // on the bots, this is really helpful in figuring out why.
  for (int i = 0; i < depth; i++)
    printf("  ");
  printf("role=%s name=%s\n",
         base::WideToUTF8(IAccessibleRoleToString(V_I4(role.ptr()))).c_str(),
         base::WideToUTF8(name).c_str());

  if (expected_role == V_I4(role.ptr()) && expected_name == name) {
    *found = true;
    return;
  }

  std::vector<base::win::ScopedVariant> children =
      GetAllAccessibleChildren(node);
  for (size_t i = 0; i < children.size(); ++i) {
    Microsoft::WRL::ComPtr<IAccessible> child_accessible(
        GetAccessibleFromVariant(node, children[i].AsInput()));
    if (child_accessible) {
      FindNodeInAccessibilityTree(child_accessible.Get(), expected_role,
                                  expected_name, depth + 1, found);
      if (*found)
        return;
    }
  }
}

// Ensures that the text and the start and end offsets retrieved using
// get_textAtOffset match the expected values.
void AccessibilityWinBrowserTest::CheckTextAtOffset(
    Microsoft::WRL::ComPtr<IAccessibleText>& object,
    LONG offset,
    IA2TextBoundaryType boundary_type,
    LONG expected_start_offset,
    LONG expected_end_offset,
    const std::wstring& expected_text) {
  testing::Message message;
  message << "While checking for \'" << expected_text << "\' at "
          << expected_start_offset << '-' << expected_end_offset << '.';
  SCOPED_TRACE(message);

  LONG start_offset = 0;
  LONG end_offset = 0;
  base::win::ScopedBstr text;
  HRESULT hr = object->get_textAtOffset(offset, boundary_type, &start_offset,
                                        &end_offset, text.Receive());
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(expected_start_offset, start_offset);
  EXPECT_EQ(expected_end_offset, end_offset);
  EXPECT_STREQ(expected_text.c_str(), text);
}

std::vector<base::win::ScopedVariant>
AccessibilityWinBrowserTest::GetAllAccessibleChildren(IAccessible* element) {
  LONG child_count = 0;
  HRESULT hr = element->get_accChildCount(&child_count);
  EXPECT_EQ(S_OK, hr);
  if (child_count <= 0)
    return std::vector<base::win::ScopedVariant>();

  std::unique_ptr<VARIANT[]> children_array(new VARIANT[child_count]);
  LONG obtained_count = 0;
  hr = AccessibleChildren(element, 0, child_count, children_array.get(),
                          &obtained_count);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(child_count, obtained_count);

  std::vector<base::win::ScopedVariant> children(
      static_cast<size_t>(child_count));
  for (size_t i = 0; i < children.size(); i++)
    children[i].Reset(children_array[i]);

  return children;
}

// AccessibleChecker ----------------------------------------------------------

class AccessibilityWinBrowserTest::AccessibleChecker {
 public:
  // This constructor can be used if the IA2 role will be the same as the MSAA
  // role.
  AccessibleChecker(const std::wstring& expected_name,
                    int32_t expected_role,
                    const std::wstring& expected_value);
  AccessibleChecker(const std::wstring& expected_name,
                    int32_t expected_role,
                    int32_t expected_ia2_role,
                    const std::wstring& expected_value);
  AccessibleChecker(const std::wstring& expected_name,
                    const std::wstring& expected_role,
                    int32_t expected_ia2_role,
                    const std::wstring& expected_value);

  // Append an AccessibleChecker that verifies accessibility information for
  // a child IAccessible. Order is important.
  void AppendExpectedChild(AccessibleChecker* expected_child);

  // Check that the name and role of the given IAccessible instance and its
  // descendants match the expected names and roles that this object was
  // initialized with.
  void CheckAccessible(IAccessible* accessible);

  // Set the expected value for this AccessibleChecker.
  void SetExpectedValue(const std::wstring& expected_value);

  // Set the expected state for this AccessibleChecker.
  void SetExpectedState(LONG expected_state);

 private:
  typedef std::vector<AccessibleChecker*> AccessibleCheckerVector;

  void CheckAccessibleName(IAccessible* accessible);
  void CheckAccessibleRole(IAccessible* accessible);
  void CheckIA2Role(IAccessible* accessible);
  void CheckAccessibleValue(IAccessible* accessible);
  void CheckAccessibleState(IAccessible* accessible);
  void CheckAccessibleChildren(IAccessible* accessible);
  base::string16 RoleVariantToString(const base::win::ScopedVariant& role);

  // Expected accessible name. Checked against IAccessible::get_accName.
  std::wstring name_;

  // Expected accessible role. Checked against IAccessible::get_accRole.
  base::win::ScopedVariant role_;

  // Expected IAccessible2 role. Checked against IAccessible2::role.
  int32_t ia2_role_;

  // Expected accessible value. Checked against IAccessible::get_accValue.
  std::wstring value_;

  // Expected accessible state. Checked against IAccessible::get_accState.
  LONG state_;

  // Expected accessible children. Checked using IAccessible::get_accChildCount
  // and ::AccessibleChildren.
  AccessibleCheckerVector children_;
};

AccessibilityWinBrowserTest::AccessibleChecker::AccessibleChecker(
    const std::wstring& expected_name,
    int32_t expected_role,
    const std::wstring& expected_value)
    : name_(expected_name),
      role_(expected_role),
      ia2_role_(expected_role),
      value_(expected_value),
      state_(-1) {}

AccessibilityWinBrowserTest::AccessibleChecker::AccessibleChecker(
    const std::wstring& expected_name,
    int32_t expected_role,
    int32_t expected_ia2_role,
    const std::wstring& expected_value)
    : name_(expected_name),
      role_(expected_role),
      ia2_role_(expected_ia2_role),
      value_(expected_value),
      state_(-1) {}

AccessibilityWinBrowserTest::AccessibleChecker::AccessibleChecker(
    const std::wstring& expected_name,
    const std::wstring& expected_role,
    int32_t expected_ia2_role,
    const std::wstring& expected_value)
    : name_(expected_name),
      role_(expected_role.c_str()),
      ia2_role_(expected_ia2_role),
      value_(expected_value),
      state_(-1) {}

void AccessibilityWinBrowserTest::AccessibleChecker::AppendExpectedChild(
    AccessibleChecker* expected_child) {
  children_.push_back(expected_child);
}

void AccessibilityWinBrowserTest::AccessibleChecker::CheckAccessible(
    IAccessible* accessible) {
  SCOPED_TRACE("While checking " +
               base::UTF16ToUTF8(RoleVariantToString(role_)));
  CheckAccessibleName(accessible);
  CheckAccessibleRole(accessible);
  CheckIA2Role(accessible);
  CheckAccessibleValue(accessible);
  CheckAccessibleState(accessible);
  CheckAccessibleChildren(accessible);
}

void AccessibilityWinBrowserTest::AccessibleChecker::SetExpectedValue(
    const std::wstring& expected_value) {
  value_ = expected_value;
}

void AccessibilityWinBrowserTest::AccessibleChecker::SetExpectedState(
    LONG expected_state) {
  state_ = expected_state;
}

void AccessibilityWinBrowserTest::AccessibleChecker::CheckAccessibleName(
    IAccessible* accessible) {
  base::win::ScopedBstr name;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  HRESULT hr = accessible->get_accName(childid_self, name.Receive());

  if (name_.empty()) {
    // If the object doesn't have name S_FALSE should be returned.
    EXPECT_EQ(S_FALSE, hr);
  } else {
    // Test that the correct string was returned.
    EXPECT_EQ(S_OK, hr);
    EXPECT_EQ(name_, std::wstring(name, name.Length()));
  }
}

void AccessibilityWinBrowserTest::AccessibleChecker::CheckAccessibleRole(
    IAccessible* accessible) {
  base::win::ScopedVariant role;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  HRESULT hr = accessible->get_accRole(childid_self, role.Receive());
  ASSERT_EQ(S_OK, hr);
  EXPECT_EQ(0, role_.Compare(role))
      << "Expected role: " << RoleVariantToString(role_)
      << "\nGot role: " << RoleVariantToString(role);
}

void AccessibilityWinBrowserTest::AccessibleChecker::CheckIA2Role(
    IAccessible* accessible) {
  Microsoft::WRL::ComPtr<IAccessible2> accessible2;
  HRESULT hr = QueryIAccessible2(accessible, accessible2.GetAddressOf());
  ASSERT_EQ(S_OK, hr);
  LONG ia2_role = 0;
  hr = accessible2->role(&ia2_role);
  ASSERT_EQ(S_OK, hr);
  EXPECT_EQ(ia2_role_, ia2_role)
      << "Expected ia2 role: " << IAccessible2RoleToString(ia2_role_)
      << "\nGot ia2 role: " << IAccessible2RoleToString(ia2_role);
}

void AccessibilityWinBrowserTest::AccessibleChecker::CheckAccessibleValue(
    IAccessible* accessible) {
  // Don't check the value if if's a DOCUMENT role, because the value
  // is supposed to be the url (and we don't keep track of that in the
  // test expectations).
  base::win::ScopedVariant role;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  HRESULT hr = accessible->get_accRole(childid_self, role.Receive());
  ASSERT_EQ(S_OK, hr);
  if (role.type() == VT_I4 && V_I4(role.ptr()) == ROLE_SYSTEM_DOCUMENT)
    return;

  // Get the value.
  base::win::ScopedBstr value;
  hr = accessible->get_accValue(childid_self, value.Receive());
  EXPECT_EQ(S_OK, hr);

  // Test that the correct string was returned.
  EXPECT_EQ(value_, std::wstring(value, value.Length()));
}

void AccessibilityWinBrowserTest::AccessibleChecker::CheckAccessibleState(
    IAccessible* accessible) {
  if (state_ < 0)
    return;

  base::win::ScopedVariant state;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  HRESULT hr = accessible->get_accState(childid_self, state.Receive());
  EXPECT_EQ(S_OK, hr);
  ASSERT_EQ(VT_I4, state.type());
  LONG obj_state = V_I4(state.ptr());
  // Avoid flakiness. The "offscreen" state depends on whether the browser
  // window is frontmost or not, and "hottracked" depends on whether the
  // mouse cursor happens to be over the element.
  obj_state &= ~(STATE_SYSTEM_OFFSCREEN | STATE_SYSTEM_HOTTRACKED);
  EXPECT_EQ(state_, obj_state)
      << "Expected state: " << IAccessibleStateToString(state_)
      << "\nGot state: " << IAccessibleStateToString(obj_state);
}

void AccessibilityWinBrowserTest::AccessibleChecker::CheckAccessibleChildren(
    IAccessible* parent) {
  std::vector<base::win::ScopedVariant> obtained_children =
      GetAllAccessibleChildren(parent);
  size_t child_count = obtained_children.size();
  ASSERT_EQ(child_count, children_.size());

  AccessibleCheckerVector::iterator child_checker;
  std::vector<base::win::ScopedVariant>::iterator child;
  for (child_checker = children_.begin(), child = obtained_children.begin();
       child_checker != children_.end() && child != obtained_children.end();
       ++child_checker, ++child) {
    Microsoft::WRL::ComPtr<IAccessible> child_accessible(
        GetAccessibleFromVariant(parent, child->AsInput()));
    ASSERT_TRUE(child_accessible.Get());
    (*child_checker)->CheckAccessible(child_accessible.Get());
  }
}

base::string16
AccessibilityWinBrowserTest::AccessibleChecker::RoleVariantToString(
    const base::win::ScopedVariant& role) {
  if (role.type() == VT_I4)
    return IAccessibleRoleToString(V_I4(role.ptr()));
  if (role.type() == VT_BSTR)
    return base::string16(V_BSTR(role.ptr()), SysStringLen(V_BSTR(role.ptr())));
  return base::string16();
}

// Helper class that listens to native Windows events using
// AccessibilityEventRecorder, and blocks until the pretty-printed
// event string matches the given match pattern.
class NativeWinEventWaiter {
 public:
  NativeWinEventWaiter(BrowserAccessibilityManager* manager,
                       const std::string& match_pattern)
      : event_recorder_(
            AccessibilityEventRecorder::Create(manager,
                                               base::GetCurrentProcId())),
        match_pattern_(match_pattern) {
    event_recorder_->ListenToEvents(base::BindRepeating(
        &NativeWinEventWaiter::OnEvent, base::Unretained(this)));
  }

  void OnEvent(const std::string& event_str) {
    DLOG(INFO) << "Got event " + event_str;
    if (base::MatchPattern(event_str, match_pattern_))
      run_loop_.Quit();
  }

  void Wait() { run_loop_.Run(); }

 private:
  std::unique_ptr<AccessibilityEventRecorder> event_recorder_;
  std::string match_pattern_;
  base::RunLoop run_loop_;
};

// Helper class that reproduces a specific crash when UIA parent navigation
// is performed during the destruction of its WebContents.
class WebContentsUIAParentNavigationInDestroyedWatcher
    : public WebContentsObserver {
 public:
  explicit WebContentsUIAParentNavigationInDestroyedWatcher(
      WebContents* web_contents,
      IUIAutomationElement* root,
      IUIAutomationTreeWalker* tree_walker)
      : WebContentsObserver(web_contents),
        root_(root),
        tree_walker_(tree_walker) {
    CHECK(web_contents);
  }

  ~WebContentsUIAParentNavigationInDestroyedWatcher() override {}

  // Waits until the WebContents is destroyed.
  void Wait() { run_loop_.Run(); }

 private:
  // Overridden WebContentsObserver methods.
  void WebContentsDestroyed() override {
    // Test navigating to the parent node via UIA
    Microsoft::WRL::ComPtr<IUIAutomationElement> parent;
    tree_walker_->GetParentElement(root_.Get(), &parent);
    CHECK(parent.Get() == nullptr);

    run_loop_.Quit();
  }

  Microsoft::WRL::ComPtr<IUIAutomationElement> root_;
  Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> tree_walker_;
  base::RunLoop run_loop_;
};

}  // namespace

// Tests ----------------------------------------------------------------------

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestBusyAccessibilityTree) {
  EXPECT_TRUE(NavigateToURL(shell(), GURL(url::kAboutBlankURL)));

  // The initial accessible returned should have state STATE_SYSTEM_BUSY while
  // the accessibility tree is being requested from the renderer.
  AccessibleChecker document1_checker(std::wstring(), ROLE_SYSTEM_DOCUMENT,
                                      std::wstring());
  document1_checker.SetExpectedState(STATE_SYSTEM_READONLY |
                                     STATE_SYSTEM_FOCUSABLE |
                                     STATE_SYSTEM_FOCUSED | STATE_SYSTEM_BUSY);
  document1_checker.CheckAccessible(GetRendererAccessible());
}

// Periodically failing.  See crbug.com/145537
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       DISABLED_TestNotificationActiveDescendantChanged) {
  LoadInitialAccessibilityTreeFromHtml(
      "<ul tabindex='-1' role='radiogroup' aria-label='ul'>"
      "<li id='li'>li</li></ul>");

  // Check the browser's copy of the renderer accessibility tree.
  AccessibleChecker list_marker_checker(L"\x2022", ROLE_SYSTEM_TEXT,
                                        std::wstring());
  AccessibleChecker static_text_checker(L"li", ROLE_SYSTEM_TEXT,
                                        std::wstring());
  AccessibleChecker list_item_checker(std::wstring(), ROLE_SYSTEM_LISTITEM,
                                      std::wstring());
  list_item_checker.SetExpectedState(STATE_SYSTEM_READONLY);
  AccessibleChecker radio_group_checker(L"ul", ROLE_SYSTEM_GROUPING,
                                        IA2_ROLE_SECTION, std::wstring());
  radio_group_checker.SetExpectedState(STATE_SYSTEM_FOCUSABLE);
  AccessibleChecker document_checker(std::wstring(), ROLE_SYSTEM_DOCUMENT,
                                     std::wstring());
  list_item_checker.AppendExpectedChild(&list_marker_checker);
  list_item_checker.AppendExpectedChild(&static_text_checker);
  radio_group_checker.AppendExpectedChild(&list_item_checker);
  document_checker.AppendExpectedChild(&radio_group_checker);
  document_checker.CheckAccessible(GetRendererAccessible());

  // Set focus to the radio group.
  auto waiter = std::make_unique<AccessibilityNotificationWaiter>(
      shell()->web_contents(), ui::kAXModeComplete, ax::mojom::Event::kFocus);
  ExecuteScript(L"document.body.children[0].focus();");
  waiter->WaitForNotification();

  // Check that the accessibility tree of the browser has been updated.
  radio_group_checker.SetExpectedState(STATE_SYSTEM_FOCUSABLE |
                                       STATE_SYSTEM_FOCUSED);
  document_checker.CheckAccessible(GetRendererAccessible());

  // Set the active descendant of the radio group
  waiter.reset(new AccessibilityNotificationWaiter(
      shell()->web_contents(), ui::kAXModeComplete, ax::mojom::Event::kFocus));
  ExecuteScript(
      L"document.body.children[0].setAttribute('aria-activedescendant', 'li')");
  waiter->WaitForNotification();

  // Check that the accessibility tree of the browser has been updated.
  list_item_checker.SetExpectedState(STATE_SYSTEM_READONLY |
                                     STATE_SYSTEM_FOCUSED);
  radio_group_checker.SetExpectedState(STATE_SYSTEM_FOCUSABLE);
  document_checker.CheckAccessible(GetRendererAccessible());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestNotificationCheckedStateChanged) {
  LoadInitialAccessibilityTreeFromHtml(
      "<body><input type='checkbox' /></body>");

  // Check the browser's copy of the renderer accessibility tree.
  AccessibleChecker checkbox_checker(std::wstring(), ROLE_SYSTEM_CHECKBUTTON,
                                     std::wstring());
  checkbox_checker.SetExpectedState(STATE_SYSTEM_FOCUSABLE);
  AccessibleChecker body_checker(std::wstring(), L"BODY", IA2_ROLE_SECTION,
                                 std::wstring());
  AccessibleChecker document_checker(std::wstring(), ROLE_SYSTEM_DOCUMENT,
                                     std::wstring());
  body_checker.AppendExpectedChild(&checkbox_checker);
  document_checker.AppendExpectedChild(&body_checker);
  document_checker.CheckAccessible(GetRendererAccessible());

  // Check the checkbox.
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kCheckedStateChanged);
  ExecuteScript(L"document.body.children[0].checked=true");
  waiter.WaitForNotification();

  // Check that the accessibility tree of the browser has been updated.
  checkbox_checker.SetExpectedState(STATE_SYSTEM_CHECKED |
                                    STATE_SYSTEM_FOCUSABLE);
  document_checker.CheckAccessible(GetRendererAccessible());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestNotificationChildrenChanged) {
  // The role attribute causes the node to be in the accessibility tree.
  LoadInitialAccessibilityTreeFromHtml("<body role=group></body>");

  // Check the browser's copy of the renderer accessibility tree.
  AccessibleChecker group_checker(std::wstring(), ROLE_SYSTEM_GROUPING,
                                  std::wstring());
  AccessibleChecker document_checker(std::wstring(), ROLE_SYSTEM_DOCUMENT,
                                     std::wstring());
  document_checker.AppendExpectedChild(&group_checker);
  document_checker.CheckAccessible(GetRendererAccessible());

  // Change the children of the document body.
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kChildrenChanged);
  ExecuteScript(L"document.body.innerHTML='<b>new text</b>'");
  waiter.WaitForNotification();

  // Check that the accessibility tree of the browser has been updated.
  AccessibleChecker text_checker(L"new text", ROLE_SYSTEM_STATICTEXT,
                                 std::wstring());
  group_checker.AppendExpectedChild(&text_checker);
  document_checker.CheckAccessible(GetRendererAccessible());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestNotificationChildrenChanged2) {
  // The role attribute causes the node to be in the accessibility tree.
  LoadInitialAccessibilityTreeFromHtml(
      "<div role=group style='visibility: hidden'>text</div>");

  // Check the accessible tree of the browser.
  AccessibleChecker document_checker(std::wstring(), ROLE_SYSTEM_DOCUMENT,
                                     std::wstring());
  document_checker.CheckAccessible(GetRendererAccessible());

  // Change the children of the document body.
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kChildrenChanged);
  ExecuteScript(L"document.body.children[0].style.visibility='visible'");
  waiter.WaitForNotification();

  // Check that the accessibility tree of the browser has been updated.
  AccessibleChecker static_text_checker(L"text", ROLE_SYSTEM_STATICTEXT,
                                        std::wstring());
  AccessibleChecker group_checker(std::wstring(), ROLE_SYSTEM_GROUPING,
                                  std::wstring());
  document_checker.AppendExpectedChild(&group_checker);
  group_checker.AppendExpectedChild(&static_text_checker);
  document_checker.CheckAccessible(GetRendererAccessible());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestNotificationFocusChanged) {
  // The role attribute causes the node to be in the accessibility tree.
  LoadInitialAccessibilityTreeFromHtml(R"HTML(
      <div role="group" tabindex="-1">
      </div>)HTML");

  // Check the browser's copy of the renderer accessibility tree.
  SCOPED_TRACE("Check initial tree");
  AccessibleChecker group_checker(std::wstring(), ROLE_SYSTEM_GROUPING,
                                  std::wstring());
  group_checker.SetExpectedState(STATE_SYSTEM_FOCUSABLE);
  AccessibleChecker document_checker(std::wstring(), ROLE_SYSTEM_DOCUMENT,
                                     std::wstring());
  document_checker.AppendExpectedChild(&group_checker);
  document_checker.CheckAccessible(GetRendererAccessible());

  {
    // Focus the div in the document
    AccessibilityNotificationWaiter waiter(
        shell()->web_contents(), ui::kAXModeComplete, ax::mojom::Event::kFocus);
    ExecuteScript(L"document.body.children[0].focus();");
    waiter.WaitForNotification();
  }

  // Check that the accessibility tree of the browser has been updated.
  SCOPED_TRACE("Check updated tree after focusing div");
  group_checker.SetExpectedState(STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_FOCUSED);
  document_checker.CheckAccessible(GetRendererAccessible());

  {
    // Focus the document accessible. This will un-focus the current node.
    AccessibilityNotificationWaiter waiter(
        shell()->web_contents(), ui::kAXModeComplete, ax::mojom::Event::kBlur);
    Microsoft::WRL::ComPtr<IAccessible> document_accessible(
        GetRendererAccessible());
    ASSERT_NE(nullptr, document_accessible.Get());
    base::win::ScopedVariant childid_self(CHILDID_SELF);
    HRESULT hr =
        document_accessible->accSelect(SELFLAG_TAKEFOCUS, childid_self);
    ASSERT_EQ(S_OK, hr);
    waiter.WaitForNotification();
  }

  // Check that the accessibility tree of the browser has been updated.
  SCOPED_TRACE("Check updated tree after focusing document again");
  group_checker.SetExpectedState(STATE_SYSTEM_FOCUSABLE);
  document_checker.CheckAccessible(GetRendererAccessible());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, FocusEventOnPageLoad) {
  // Some screen readers, such as older versions of Jaws, require a focus event
  // on the top document after the page loads, if there is no focused element on
  // the page.
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kLoadComplete);
  GURL html_data_url(
      "data:text/html," +
      net::EscapeQueryParamValue(R"HTML(<p> Hello</ p>)HTML", false));
  EXPECT_TRUE(NavigateToURL(shell(), html_data_url));
  WaitForAccessibilityFocusChange();
  waiter.WaitForNotification();

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  ASSERT_TRUE(document);
  base::win::ScopedVariant focus;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  ASSERT_HRESULT_SUCCEEDED(document->get_accFocus(focus.Receive()));
  EXPECT_EQ(VT_I4, focus.type());
  EXPECT_EQ(CHILDID_SELF, V_I4(focus.ptr()));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, NoFocusEventOnRootChange) {
  LoadInitialAccessibilityTreeFromHtml(R"HTML(
      <p>Hello</p>
      )HTML");

  // Adding an iframe as a direct descendant of the root will reserialize the
  // root node.
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kLayoutComplete);
  ExecuteScript(
      L"let iframe = document.createElement('iframe');"
      L"iframe.srcdoc = '<button>Button</button>';"
      L"document.body.appendChild(iframe);");
  waiter.WaitForNotification();

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  ASSERT_TRUE(document);
  base::win::ScopedVariant focus;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  ASSERT_HRESULT_SUCCEEDED(document->get_accFocus(focus.Receive()));
  EXPECT_EQ(VT_I4, focus.type());
  EXPECT_EQ(CHILDID_SELF, V_I4(focus.ptr()));
}

// Flaky on win crbug.com/979741
#if defined(OS_WIN)
#define MAYBE_FocusEventOnFocusedIframeAddedAndRemoved \
  DISABLED_FocusEventOnFocusedIframeAddedAndRemoved
#else
#define MAYBE_FocusEventOnFocusedIframeAddedAndRemoved \
  FocusEventOnFocusedIframeAddedAndRemoved
#endif
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       MAYBE_FocusEventOnFocusedIframeAddedAndRemoved) {
  LoadInitialAccessibilityTreeFromHtml(R"HTML(
      <button autofocus>Outer button</button>
      )HTML");

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  ASSERT_TRUE(document);

  {
    AccessibilityNotificationWaiter iframe_waiter(
        shell()->web_contents(), ui::kAXModeComplete, ax::mojom::Event::kFocus);
    ExecuteScript(
        L"let iframe = document.createElement('iframe');"
        L"iframe.srcdoc = '<button autofocus>Inner button</button>';"
        L"document.body.appendChild(iframe);");
    WaitForAccessibilityFocusChange();
    iframe_waiter.WaitForNotification();

    const BrowserAccessibility* inner_button =
        FindNode(ax::mojom::Role::kButton, "Inner button");
    ASSERT_NE(nullptr, inner_button);
    const auto* inner_button_win =
        ToBrowserAccessibilityWin(inner_button)->GetCOM();
    ASSERT_NE(nullptr, inner_button_win);

    base::win::ScopedVariant focus;
    base::win::ScopedVariant childid_self(CHILDID_SELF);
    ASSERT_HRESULT_SUCCEEDED(document->get_accFocus(focus.Receive()));
    EXPECT_EQ(VT_DISPATCH, focus.type());
    EXPECT_EQ(inner_button_win, V_DISPATCH(focus.ptr()));
  }

  {
    AccessibilityNotificationWaiter iframe_waiter(
        shell()->web_contents(), ui::kAXModeComplete,
        ax::mojom::Event::kLayoutComplete);
    ExecuteScript(L"document.body.removeChild(iframe);");
    WaitForAccessibilityFocusChange();
    iframe_waiter.WaitForNotification();

    base::win::ScopedVariant focus;
    base::win::ScopedVariant childid_self(CHILDID_SELF);
    ASSERT_HRESULT_SUCCEEDED(document->get_accFocus(focus.Receive()));
    EXPECT_EQ(VT_I4, focus.type());
    EXPECT_EQ(CHILDID_SELF, V_I4(focus.ptr()));
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       NoFocusEventOnIframeAddedAndRemoved) {
  LoadInitialAccessibilityTreeFromHtml(R"HTML(
      <button autofocus>Outer button</button>
      )HTML");

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  ASSERT_TRUE(document);

  const BrowserAccessibility* outer_button =
      FindNode(ax::mojom::Role::kButton, "Outer button");
  ASSERT_NE(nullptr, outer_button);
  const auto* outer_button_win =
      ToBrowserAccessibilityWin(outer_button)->GetCOM();
  ASSERT_NE(nullptr, outer_button_win);

  {
    AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                           ui::kAXModeComplete,
                                           ax::mojom::Event::kLayoutComplete);
    ExecuteScript(
        L"let iframe = document.createElement('iframe');"
        L"iframe.srcdoc = '<button>Inner button</button>';"
        L"document.body.appendChild(iframe);");
    waiter.WaitForNotification();

    base::win::ScopedVariant focus;
    base::win::ScopedVariant childid_self(CHILDID_SELF);
    ASSERT_HRESULT_SUCCEEDED(document->get_accFocus(focus.Receive()));
    EXPECT_EQ(VT_DISPATCH, focus.type());
    EXPECT_EQ(outer_button_win, V_DISPATCH(focus.ptr()));
  }

  {
    AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                           ui::kAXModeComplete,
                                           ax::mojom::Event::kLayoutComplete);
    ExecuteScript(L"document.body.removeChild(iframe);");
    waiter.WaitForNotification();

    base::win::ScopedVariant focus;
    base::win::ScopedVariant childid_self(CHILDID_SELF);
    ASSERT_HRESULT_SUCCEEDED(document->get_accFocus(focus.Receive()));
    EXPECT_EQ(VT_DISPATCH, focus.type());
    EXPECT_EQ(outer_button_win, V_DISPATCH(focus.ptr()));
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestNotificationValueChanged) {
  LoadInitialAccessibilityTreeFromHtml(
      "<body><input type='text' value='old value'/></body>");

  // Check the browser's copy of the renderer accessibility tree.
  AccessibleChecker text_field_checker(std::wstring(), ROLE_SYSTEM_TEXT,
                                       L"old value");
  text_field_checker.SetExpectedState(STATE_SYSTEM_FOCUSABLE);
  AccessibleChecker body_checker(std::wstring(), L"BODY", IA2_ROLE_SECTION,
                                 std::wstring());
  AccessibleChecker document_checker(std::wstring(), ROLE_SYSTEM_DOCUMENT,
                                     std::wstring());
  body_checker.AppendExpectedChild(&text_field_checker);
  document_checker.AppendExpectedChild(&body_checker);
  document_checker.CheckAccessible(GetRendererAccessible());

  // Set the value of the text control
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kValueChanged);
  ExecuteScript(L"document.body.children[0].value='new value'");
  waiter.WaitForNotification();

  // Check that the accessibility tree of the browser has been updated.
  text_field_checker.SetExpectedValue(L"new value");
  document_checker.CheckAccessible(GetRendererAccessible());
}

// This test verifies that the web content's accessibility tree is a
// descendant of the main browser window's accessibility tree, so that
// tools like AccExplorer32 or AccProbe can be used to examine Chrome's
// accessibility support.
//
// If you made a change and this test now fails, check that the NativeViewHost
// that wraps the tab contents returns the IAccessible implementation
// provided by RenderWidgetHostViewWin in GetNativeViewAccessible().
// flaky: http://crbug.com/402190
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       DISABLED_ContainsRendererAccessibilityTree) {
  LoadInitialAccessibilityTreeFromHtml(
      "<html><head><title>MyDocument</title></head>"
      "<body>Content</body></html>");

  // Get the accessibility object for the window tree host.
  aura::Window* window = shell()->window();
  CHECK(window);
  aura::WindowTreeHost* window_tree_host = window->GetHost();
  CHECK(window_tree_host);
  HWND hwnd = window_tree_host->GetAcceleratedWidget();
  CHECK(hwnd);
  Microsoft::WRL::ComPtr<IAccessible> browser_accessible;
  HRESULT hr = AccessibleObjectFromWindow(
      hwnd, OBJID_WINDOW, IID_IAccessible,
      reinterpret_cast<void**>(browser_accessible.GetAddressOf()));
  ASSERT_EQ(S_OK, hr);

  bool found = false;
  FindNodeInAccessibilityTree(browser_accessible.Get(), ROLE_SYSTEM_DOCUMENT,
                              L"MyDocument", 0, &found);
  ASSERT_EQ(found, true);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, SupportsISimpleDOM) {
  LoadInitialAccessibilityTreeFromHtml("<body><input type='checkbox'></body>");

  // Get the IAccessible object for the document.
  Microsoft::WRL::ComPtr<IAccessible> document_accessible(
      GetRendererAccessible());
  ASSERT_NE(document_accessible.Get(), reinterpret_cast<IAccessible*>(NULL));

  // Get the ISimpleDOM object for the document.
  Microsoft::WRL::ComPtr<IServiceProvider> service_provider;
  HRESULT hr = static_cast<IAccessible*>(document_accessible.Get())
                   ->QueryInterface(service_provider.GetAddressOf());
  ASSERT_EQ(S_OK, hr);
  const GUID refguid = {0x0c539790,
                        0x12e4,
                        0x11cf,
                        {0xb6, 0x61, 0x00, 0xaa, 0x00, 0x4c, 0xd6, 0xd8}};
  Microsoft::WRL::ComPtr<ISimpleDOMNode> document_isimpledomnode;
  hr = service_provider->QueryService(refguid,
                                      IID_PPV_ARGS(&document_isimpledomnode));
  ASSERT_EQ(S_OK, hr);

  base::win::ScopedBstr node_name;
  short name_space_id;  // NOLINT
  base::win::ScopedBstr node_value;
  unsigned int num_children;
  unsigned int unique_id;
  unsigned short node_type;  // NOLINT
  hr = document_isimpledomnode->get_nodeInfo(
      node_name.Receive(), &name_space_id, node_value.Receive(), &num_children,
      &unique_id, &node_type);
  ASSERT_EQ(S_OK, hr);
  EXPECT_EQ(NODETYPE_DOCUMENT, node_type);
  EXPECT_EQ(1u, num_children);
  node_name.Reset();
  node_value.Reset();

  Microsoft::WRL::ComPtr<ISimpleDOMNode> body_isimpledomnode;
  hr = document_isimpledomnode->get_firstChild(
      body_isimpledomnode.GetAddressOf());
  ASSERT_EQ(S_OK, hr);
  hr = body_isimpledomnode->get_nodeInfo(node_name.Receive(), &name_space_id,
                                         node_value.Receive(), &num_children,
                                         &unique_id, &node_type);
  ASSERT_EQ(S_OK, hr);
  EXPECT_EQ(L"body", std::wstring(node_name, node_name.Length()));
  EXPECT_EQ(NODETYPE_ELEMENT, node_type);
  EXPECT_EQ(1u, num_children);
  node_name.Reset();
  node_value.Reset();

  Microsoft::WRL::ComPtr<ISimpleDOMNode> checkbox_isimpledomnode;
  hr = body_isimpledomnode->get_firstChild(
      checkbox_isimpledomnode.GetAddressOf());
  ASSERT_EQ(S_OK, hr);
  hr = checkbox_isimpledomnode->get_nodeInfo(
      node_name.Receive(), &name_space_id, node_value.Receive(), &num_children,
      &unique_id, &node_type);
  ASSERT_EQ(S_OK, hr);
  EXPECT_EQ(L"input", std::wstring(node_name, node_name.Length()));
  EXPECT_EQ(NODETYPE_ELEMENT, node_type);
  EXPECT_EQ(0u, num_children);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestRoleGroup) {
  LoadInitialAccessibilityTreeFromHtml(
      "<fieldset></fieldset><div role=group></div>");

  // Check the browser's copy of the renderer accessibility tree.
  AccessibleChecker grouping1_checker(std::wstring(), ROLE_SYSTEM_GROUPING,
                                      std::wstring());
  AccessibleChecker grouping2_checker(std::wstring(), ROLE_SYSTEM_GROUPING,
                                      std::wstring());
  AccessibleChecker document_checker(std::wstring(), ROLE_SYSTEM_DOCUMENT,
                                     std::wstring());
  document_checker.AppendExpectedChild(&grouping1_checker);
  document_checker.AppendExpectedChild(&grouping2_checker);
  document_checker.CheckAccessible(GetRendererAccessible());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestAccSelectionWithNoSelectedItems) {
  LoadInitialAccessibilityTreeFromHtml(R"HTML(
<div role="listbox" aria-expanded="true">
<div role="option" aria-selected="false">
Option 1
</div>
<div role="option" aria-selected="false">
Option 2
</div>
<div aria-selected="false">
Option 3
</div>
</div>
      )HTML");

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  ASSERT_TRUE(document);
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> listbox;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      &listbox));
  LONG listbox_role = 0;
  ASSERT_HRESULT_SUCCEEDED(listbox->role(&listbox_role));
  ASSERT_EQ(ROLE_SYSTEM_LIST, listbox_role);

  base::win::ScopedVariant selected;
  ASSERT_HRESULT_SUCCEEDED(listbox->get_accSelection(selected.Receive()));
  EXPECT_EQ(VT_EMPTY, selected.type());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestAccSelectionWithOneSelectedItem) {
  LoadInitialAccessibilityTreeFromHtml(R"HTML(
<div role="listbox" aria-expanded="true">
<div role="option" aria-selected="false">
Option 1
</div>
<div role="option" aria-selected="true">
Option 2
</div>
<div aria-selected="false">
Option 3
</div>
</div>
      )HTML");

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  ASSERT_TRUE(document);
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> listbox;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      &listbox));
  LONG listbox_role = 0;
  ASSERT_HRESULT_SUCCEEDED(listbox->role(&listbox_role));
  ASSERT_EQ(ROLE_SYSTEM_LIST, listbox_role);

  base::win::ScopedVariant selected;
  ASSERT_HRESULT_SUCCEEDED(listbox->get_accSelection(selected.Receive()));
  ASSERT_EQ(VT_DISPATCH, selected.type());

  base::win::ScopedVariant childid_self(CHILDID_SELF);
  Microsoft::WRL::ComPtr<IAccessible2> option;
  ASSERT_HRESULT_SUCCEEDED(
      V_DISPATCH(selected.AsInput())->QueryInterface(IID_PPV_ARGS(&option)));
  LONG option_role = 0;
  EXPECT_HRESULT_SUCCEEDED(option->role(&option_role));
  EXPECT_EQ(ROLE_SYSTEM_LISTITEM, option_role);
  base::win::ScopedBstr option_name;
  EXPECT_HRESULT_SUCCEEDED(
      option->get_accName(childid_self, option_name.Receive()));
  EXPECT_STREQ(L"Option 2", static_cast<BSTR>(option_name));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestAccSelectionWithMultipleSelectedItems) {
  LoadInitialAccessibilityTreeFromHtml(R"HTML(
<div role="listbox" aria-expanded="true">
<div role="option" aria-selected="true">
Option 1
</div>
<div role="option" aria-selected="true">
Option 2
</div>
<div aria-selected="false">
Option 3
</div>
</div>
      )HTML");

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  ASSERT_TRUE(document);
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> listbox;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      &listbox));
  LONG listbox_role = 0;
  ASSERT_HRESULT_SUCCEEDED(listbox->role(&listbox_role));
  ASSERT_EQ(ROLE_SYSTEM_LIST, listbox_role);

  base::win::ScopedVariant selected;
  ASSERT_HRESULT_SUCCEEDED(listbox->get_accSelection(selected.Receive()));
  ASSERT_EQ(VT_UNKNOWN, selected.type());

  Microsoft::WRL::ComPtr<IEnumVARIANT> enum_variant;
  ASSERT_HRESULT_SUCCEEDED(V_UNKNOWN(selected.AsInput())
                               ->QueryInterface(IID_PPV_ARGS(&enum_variant)));

  selected.Release();
  ASSERT_HRESULT_SUCCEEDED(enum_variant->Next(1, selected.Receive(), nullptr));
  ASSERT_EQ(VT_DISPATCH, selected.type());

  base::win::ScopedVariant childid_self(CHILDID_SELF);
  {
    Microsoft::WRL::ComPtr<IAccessible2> option;
    ASSERT_HRESULT_SUCCEEDED(
        V_DISPATCH(selected.AsInput())->QueryInterface(IID_PPV_ARGS(&option)));
    LONG option_role = 0;
    EXPECT_HRESULT_SUCCEEDED(option->role(&option_role));
    EXPECT_EQ(ROLE_SYSTEM_LISTITEM, option_role);
    base::win::ScopedBstr option_name;
    EXPECT_HRESULT_SUCCEEDED(
        option->get_accName(childid_self, option_name.Receive()));
    EXPECT_STREQ(L"Option 1", static_cast<BSTR>(option_name));
  }

  selected.Release();
  ASSERT_HRESULT_SUCCEEDED(enum_variant->Next(1, selected.Receive(), nullptr));
  ASSERT_EQ(VT_DISPATCH, selected.type());

  {
    Microsoft::WRL::ComPtr<IAccessible2> option;
    ASSERT_HRESULT_SUCCEEDED(
        V_DISPATCH(selected.AsInput())->QueryInterface(IID_PPV_ARGS(&option)));
    LONG option_role = 0;
    EXPECT_HRESULT_SUCCEEDED(option->role(&option_role));
    EXPECT_EQ(ROLE_SYSTEM_LISTITEM, option_role);
    base::win::ScopedBstr option_name;
    EXPECT_HRESULT_SUCCEEDED(
        option->get_accName(childid_self, option_name.Receive()));
    EXPECT_STREQ(L"Option 2", static_cast<BSTR>(option_name));
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsWithInvalidArguments) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraph(&paragraph_text);

  LONG invalid_offset = -3;
  LONG x = -1, y = -1;
  LONG width = -1, height = -1;

  HRESULT hr = paragraph_text->get_characterExtents(
      invalid_offset, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height);
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(-1, x);
  EXPECT_EQ(-1, y);
  EXPECT_EQ(-1, width);
  EXPECT_EQ(-1, height);
  hr = paragraph_text->get_characterExtents(
      invalid_offset, IA2_COORDTYPE_PARENT_RELATIVE, &x, &y, &width, &height);
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(-1, x);
  EXPECT_EQ(-1, y);
  EXPECT_EQ(-1, width);
  EXPECT_EQ(-1, height);

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_nCharacters(&n_characters));
  ASSERT_LT(0, n_characters);

  invalid_offset = n_characters + 1;
  hr = paragraph_text->get_characterExtents(
      invalid_offset, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height);
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(-1, x);
  EXPECT_EQ(-1, y);
  EXPECT_EQ(-1, width);
  EXPECT_EQ(-1, height);
  hr = paragraph_text->get_characterExtents(
      invalid_offset, IA2_COORDTYPE_PARENT_RELATIVE, &x, &y, &width, &height);
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(-1, x);
  EXPECT_EQ(-1, y);
  EXPECT_EQ(-1, width);
  EXPECT_EQ(-1, height);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsInEditable) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraph(&paragraph_text);

  constexpr LONG newline_offset = 46;
  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_nCharacters(&n_characters));
  ASSERT_EQ(105, n_characters);

  LONG x, y, width, height;
  LONG previous_x, previous_y, previous_height;
  for (int coordinate = IA2_COORDTYPE_SCREEN_RELATIVE;
       coordinate <= IA2_COORDTYPE_PARENT_RELATIVE; ++coordinate) {
    auto coordinate_type = static_cast<IA2CoordinateType>(coordinate);
    EXPECT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
        0, coordinate_type, &x, &y, &width, &height));
    EXPECT_LT(0, x) << "at offset 0";
    EXPECT_LT(0, y) << "at offset 0";
    EXPECT_LT(1, width) << "at offset 0";
    EXPECT_LT(1, height) << "at offset 0";

    for (LONG offset = 1; offset < newline_offset; ++offset) {
      previous_x = x;
      previous_y = y;
      previous_height = height;

      EXPECT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
          offset, coordinate_type, &x, &y, &width, &height));
      EXPECT_LT(previous_x, x) << "at offset " << offset;
      EXPECT_EQ(previous_y, y) << "at offset " << offset;
      EXPECT_LT(1, width) << "at offset " << offset;
      EXPECT_EQ(previous_height, height) << "at offset " << offset;
    }

    EXPECT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
        newline_offset + 1, coordinate_type, &x, &y, &width, &height));
    EXPECT_LE(0, x) << "at offset " << newline_offset + 1;
    EXPECT_GT(previous_x, x) << "at offset " << newline_offset + 1;
    EXPECT_LT(previous_y, y) << "at offset " << newline_offset + 1;
    EXPECT_LT(1, width) << "at offset " << newline_offset + 1;
    EXPECT_EQ(previous_height, height) << "at offset " << newline_offset + 1;

    for (LONG offset = newline_offset + 2; offset < n_characters; ++offset) {
      previous_x = x;
      previous_y = y;
      previous_height = height;

      EXPECT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
          offset, coordinate_type, &x, &y, &width, &height));
      EXPECT_LT(previous_x, x) << "at offset " << offset;
      EXPECT_EQ(previous_y, y) << "at offset " << offset;
      EXPECT_LT(1, width) << "at offset " << offset;
      EXPECT_EQ(previous_height, height) << "at offset " << offset;
    }

    // Past end of text.
    EXPECT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
        n_characters, coordinate_type, &x, &y, &width, &height));
    EXPECT_LT(previous_x, x) << "at final offset " << n_characters;
    EXPECT_EQ(previous_y, y) << "at final offset " << n_characters;
    // Last character width past end should be 1, the width of a caret.
    EXPECT_EQ(1, width) << "at final offset " << n_characters;
    EXPECT_EQ(previous_height, height) << "at final offset " << n_characters;
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsInScrollableEditable) {
  Microsoft::WRL::ComPtr<IAccessibleText> editable_container;
  // By construction, only the first line of the content editable is visible.
  SetUpSampleParagraphInScrollableEditable(&editable_container);

  constexpr LONG first_line_end = 5;
  constexpr LONG last_line_start = 8;
  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(editable_container->get_nCharacters(&n_characters));
  ASSERT_EQ(13, n_characters);
  LONG caret_offset;
  ASSERT_HRESULT_SUCCEEDED(editable_container->get_caretOffset(&caret_offset));
  ASSERT_EQ(last_line_start, caret_offset);

  LONG x, y, width, height;
  LONG previous_x, previous_y, previous_height;
  for (int coordinate = IA2_COORDTYPE_SCREEN_RELATIVE;
       coordinate <= IA2_COORDTYPE_PARENT_RELATIVE; ++coordinate) {
    auto coordinate_type = static_cast<IA2CoordinateType>(coordinate);

    // Test that non offscreen characters have increasing x coordinates and a
    // height that is greater than 1px.
    EXPECT_HRESULT_SUCCEEDED(editable_container->get_characterExtents(
        0, coordinate_type, &x, &y, &width, &height));
    EXPECT_LT(0, x) << "at offset 0";
    EXPECT_LT(0, y) << "at offset 0";
    EXPECT_LT(1, width) << "at offset 0";
    EXPECT_LT(1, height) << "at offset 0";

    for (LONG offset = 1; offset < first_line_end; ++offset) {
      previous_x = x;
      previous_y = y;
      previous_height = height;

      EXPECT_HRESULT_SUCCEEDED(editable_container->get_characterExtents(
          offset, coordinate_type, &x, &y, &width, &height));
      EXPECT_LT(previous_x, x) << "at offset " << offset;
      EXPECT_EQ(previous_y, y) << "at offset " << offset;
      EXPECT_LT(1, width) << "at offset " << offset;
      EXPECT_EQ(previous_height, height) << "at offset " << offset;
    }

    EXPECT_HRESULT_SUCCEEDED(editable_container->get_characterExtents(
        last_line_start, coordinate_type, &x, &y, &width, &height));
    EXPECT_LT(0, x) << "at offset " << last_line_start;
    EXPECT_LT(previous_y, y) << "at offset " << last_line_start;
    EXPECT_LT(1, width) << "at offset " << last_line_start;
    EXPECT_EQ(previous_height, height) << "at offset " << last_line_start;

    for (LONG offset = last_line_start + 1; offset < n_characters; ++offset) {
      previous_x = x;
      previous_y = y;

      EXPECT_HRESULT_SUCCEEDED(editable_container->get_characterExtents(
          offset, coordinate_type, &x, &y, &width, &height));
      EXPECT_LT(previous_x, x) << "at offset " << offset;
      EXPECT_EQ(previous_y, y) << "at offset " << offset;
      EXPECT_LT(1, width) << "at offset " << offset;
      EXPECT_EQ(previous_height, height) << "at offset " << offset;
    }
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsInScrollableInputField) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpScrollableInputField(&input_text);

  int contents_string_length = int{InputContentsString().size()};
  constexpr LONG visible_characters_start = 21;
  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(contents_string_length, n_characters);
  LONG caret_offset;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_caretOffset(&caret_offset));
  ASSERT_EQ(contents_string_length - 1, caret_offset);

  LONG x, y, width, height;
  LONG previous_x, previous_y, previous_height;
  for (int coordinate = IA2_COORDTYPE_SCREEN_RELATIVE;
       coordinate <= IA2_COORDTYPE_PARENT_RELATIVE; ++coordinate) {
    auto coordinate_type = static_cast<IA2CoordinateType>(coordinate);

    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        0, coordinate_type, &x, &y, &width, &height));
    EXPECT_GT(0, x + width) << "at offset 0";
    EXPECT_LT(0, y) << "at offset 0";
    EXPECT_LT(1, width) << "at offset 0";
    EXPECT_LT(1, height) << "at offset 0";

    for (LONG offset = 1; offset < (visible_characters_start - 1); ++offset) {
      previous_x = x;
      previous_y = y;
      previous_height = height;

      EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
          offset, coordinate_type, &x, &y, &width, &height));
      EXPECT_LT(previous_x, x) << "at offset " << offset;
      EXPECT_EQ(previous_y, y) << "at offset " << offset;
      EXPECT_LT(1, width) << "at offset " << offset;
      EXPECT_EQ(previous_height, height) << "at offset " << offset;
    }

    // Test that non offscreen characters have increasing x coordinates and a
    // width that is greater than 1px.
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        visible_characters_start, coordinate_type, &x, &y, &width, &height));
    EXPECT_LT(previous_x, x) << "at offset " << visible_characters_start;
    EXPECT_EQ(previous_y, y) << "at offset " << visible_characters_start;
    EXPECT_LT(1, width) << "at offset " << visible_characters_start;
    EXPECT_EQ(previous_height, height)
        << "at offset " << visible_characters_start;

    // Exclude the dot at the end of the text field, because it has a width of
    // one anyway.
    for (LONG offset = visible_characters_start + 1;
         offset < (n_characters - 1); ++offset) {
      previous_x = x;
      previous_y = y;
      previous_height = height;

      EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
          offset, coordinate_type, &x, &y, &width, &height));
      EXPECT_LT(previous_x, x) << "at offset " << offset;
      EXPECT_EQ(previous_y, y) << "at offset " << offset;
      EXPECT_LT(1, width) << "at offset " << offset;
      EXPECT_EQ(previous_height, height) << "at offset " << offset;
    }
    // Past end of text.
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        n_characters, coordinate_type, &x, &y, &width, &height));
    EXPECT_LT(previous_x, x) << "at final offset " << n_characters;
    EXPECT_EQ(previous_y, y) << "at final offset " << n_characters;
    // Last character width past end should be 1, the width of a caret.
    EXPECT_EQ(1, width) << "at final offset " << n_characters;
    EXPECT_EQ(previous_height, height) << "at final offset " << n_characters;
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsInEmptyInputField) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpSingleCharInputField(&input_text);

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(1, n_characters);

  // Get the rect for the only character.
  LONG prev_x, prev_y, prev_width, prev_height;
  EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
      0, IA2_COORDTYPE_SCREEN_RELATIVE, &prev_x, &prev_y, &prev_width,
      &prev_height));

  EXPECT_LT(1, prev_width);
  EXPECT_LT(1, prev_height);

  base::win::ScopedBstr text0;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_text(0, -1, text0.Receive()));

  // Delete the character in the input field.
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  ExecuteScript(std::wstring(L"document.querySelector('input').value='';"));
  waiter.WaitForNotification();

  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(0, n_characters);
  LONG caret_offset;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_caretOffset(&caret_offset));
  ASSERT_EQ(0, caret_offset);

  base::win::ScopedBstr text;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_text(0, -1, text.Receive()));

  // Now that input is completely empty, the position of the caret should be
  // returned for character 0. The x,y position and height should be the same as
  // it was as when there was single character, but the width should now be 1.
  LONG x, y, width, height;
  for (int offset = IA2_TEXT_OFFSET_CARET; offset <= 0; ++offset) {
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        offset, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
    EXPECT_EQ(prev_x, x);
    EXPECT_EQ(prev_y, y);
    EXPECT_EQ(1, width);
    EXPECT_EQ(prev_height, height);
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsInEmptyInputFieldWithPlaceholder) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpSingleCharInputFieldWithPlaceholder(&input_text);

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(1, n_characters);

  // Get the rect for the only character.
  LONG prev_x, prev_y, prev_width, prev_height;
  EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
      0, IA2_COORDTYPE_SCREEN_RELATIVE, &prev_x, &prev_y, &prev_width,
      &prev_height));

  EXPECT_LT(1, prev_width);
  EXPECT_LT(1, prev_height);

  base::win::ScopedBstr text0;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_text(0, -1, text0.Receive()));

  // Delete the character in the input field.
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  ExecuteScript(std::wstring(L"document.querySelector('input').value='';"));
  waiter.WaitForNotification();

  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(0, n_characters);
  LONG caret_offset;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_caretOffset(&caret_offset));
  ASSERT_EQ(0, caret_offset);

  base::win::ScopedBstr text;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_text(0, -1, text.Receive()));

  // Now that input is completely empty, the position of the caret should be
  // returned for character 0. The x,y position and height should be the same as
  // it was as when there was single character, but the width should now be 1.
  LONG x, y, width, height;
  for (int offset = IA2_TEXT_OFFSET_CARET; offset <= 0; ++offset) {
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        offset, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
    EXPECT_EQ(prev_x, x);
    EXPECT_EQ(prev_y, y);
    EXPECT_EQ(7, width);
    EXPECT_EQ(prev_height, height);
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsInScrollableInputTypeSearchField) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpScrollableInputTypeSearchField(&input_text);

  int contents_string_length = int{InputContentsString().size()};
  constexpr LONG visible_characters_start = 21;
  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(contents_string_length, n_characters);
  LONG caret_offset;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_caretOffset(&caret_offset));
  ASSERT_EQ(contents_string_length - 1, caret_offset);

  LONG x, y, width, height;
  LONG previous_x, previous_y, previous_height;
  for (int coordinate = IA2_COORDTYPE_SCREEN_RELATIVE;
       coordinate <= IA2_COORDTYPE_PARENT_RELATIVE; ++coordinate) {
    auto coordinate_type = static_cast<IA2CoordinateType>(coordinate);

    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        0, coordinate_type, &x, &y, &width, &height));
    EXPECT_GT(0, x + width) << "at offset 0";
    EXPECT_LT(0, y) << "at offset 0";
    EXPECT_LT(1, width) << "at offset 0";
    EXPECT_LT(1, height) << "at offset 0";

    for (LONG offset = 1; offset < (visible_characters_start - 1); ++offset) {
      previous_x = x;
      previous_y = y;
      previous_height = height;

      EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
          offset, coordinate_type, &x, &y, &width, &height));
      EXPECT_LT(previous_x, x) << "at offset " << offset;
      EXPECT_EQ(previous_y, y) << "at offset " << offset;
      EXPECT_LT(1, width) << "at offset " << offset;
      EXPECT_EQ(previous_height, height) << "at offset " << offset;
    }

    // Test that non offscreen characters have increasing x coordinates and a
    // width that is greater than 1px.
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        visible_characters_start, coordinate_type, &x, &y, &width, &height));
    EXPECT_LT(previous_x, x) << "at offset " << visible_characters_start;
    EXPECT_EQ(previous_y, y) << "at offset " << visible_characters_start;
    EXPECT_LT(1, width) << "at offset " << visible_characters_start;
    EXPECT_EQ(previous_height, height)
        << "at offset " << visible_characters_start;

    // Exclude the dot at the end of the text field, because it has a width of
    // one anyway.
    for (LONG offset = visible_characters_start + 1;
         offset < (n_characters - 1); ++offset) {
      previous_x = x;
      previous_y = y;
      previous_height = height;

      EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
          offset, coordinate_type, &x, &y, &width, &height));
      EXPECT_LT(previous_x, x) << "at offset " << offset;
      EXPECT_EQ(previous_y, y) << "at offset " << offset;
      EXPECT_LT(1, width) << "at offset " << offset;
      EXPECT_EQ(previous_height, height) << "at offset " << offset;
    }
    // Past end of text.
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        n_characters, coordinate_type, &x, &y, &width, &height));
    EXPECT_LT(previous_x, x) << "at final offset " << n_characters;
    EXPECT_EQ(previous_y, y) << "at final offset " << n_characters;
    // Last character width past end should be 1, the width of a caret.
    EXPECT_EQ(1, width) << "at final offset " << n_characters;
    EXPECT_EQ(previous_height, height) << "at final offset " << n_characters;
  }
}

// TODO(accessibility) empty contenteditable gets height of entire
// contenteditable instead of just 1 line. May be able to use the following
// in Blink to get the height of a line -- it's at least close:
// layout_object->Style()->GetFont().PrimaryFont()->GetFontMetrics().Height()
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       DISABLED_TestCharacterExtentsInEmptyContenteditable) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpSampleParagraphInScrollableEditable(&input_text);

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_LT(0, n_characters);

  // Get the rect for the only character.
  LONG prev_x, prev_y, prev_width, prev_height;
  EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
      0, IA2_COORDTYPE_SCREEN_RELATIVE, &prev_x, &prev_y, &prev_width,
      &prev_height));

  EXPECT_LT(1, prev_width);
  EXPECT_LT(1, prev_height);

  base::win::ScopedBstr text0;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_text(0, -1, text0.Receive()));

  // Delete the character in the input field.
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kChildrenChanged);
  ExecuteScript(std::wstring(
      L"document.querySelector('[contenteditable]').innerText='';"));
  waiter.WaitForNotification();

  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(0, n_characters);
  LONG caret_offset;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_caretOffset(&caret_offset));
  ASSERT_EQ(0, caret_offset);

  base::win::ScopedBstr text;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_text(0, -1, text.Receive()));

  // Now that input is completely empty, the position of the caret should be
  // returned for character 0. The x,y position and height should be the same as
  // it was as when there was single character, but the width should now be 1.
  LONG x, y, width, height;
  for (int offset = IA2_TEXT_OFFSET_CARET; offset <= 0; ++offset) {
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        offset, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
    EXPECT_EQ(prev_x, x);
    EXPECT_EQ(prev_y, y);
    EXPECT_EQ(1, width);
    EXPECT_EQ(prev_height, height);
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsInEmptyTextarea) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpSingleCharTextarea(&input_text);

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(1, n_characters);

  // Get the rect for the only character.
  LONG prev_x, prev_y, prev_width, prev_height;
  EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
      0, IA2_COORDTYPE_SCREEN_RELATIVE, &prev_x, &prev_y, &prev_width,
      &prev_height));

  EXPECT_LT(1, prev_width);
  EXPECT_LT(1, prev_height);

  base::win::ScopedBstr text0;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_text(0, -1, text0.Receive()));

  // Delete the character in the input field.
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  ExecuteScript(
      std::wstring(L"document.querySelector('textarea').innerText='';"));
  waiter.WaitForNotification();

  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(0, n_characters);
  LONG caret_offset;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_caretOffset(&caret_offset));
  ASSERT_EQ(0, caret_offset);

  base::win::ScopedBstr text;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_text(0, -1, text.Receive()));

  // Now that input is completely empty, the position of the caret should be
  // returned for character 0. The x,y position and height should be the same as
  // it was as when there was single character, but the width should now be 1.
  LONG x, y, width, height;
  for (int offset = IA2_TEXT_OFFSET_CARET; offset <= 0; ++offset) {
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        offset, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
    EXPECT_EQ(prev_x, x);
    EXPECT_EQ(prev_y, y);
    EXPECT_EQ(1, width);
    EXPECT_EQ(prev_height, height);
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsInEmptyRtlInputField) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpSingleCharRtlInputField(&input_text);

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(1, n_characters);

  // Get the rect for the only character.
  LONG prev_x, prev_y, prev_width, prev_height;
  EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
      0, IA2_COORDTYPE_SCREEN_RELATIVE, &prev_x, &prev_y, &prev_width,
      &prev_height));

  EXPECT_LT(1, prev_width);
  EXPECT_LT(1, prev_height);

  // Delete the character in the input field.
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  ExecuteScript(
      std::wstring(L"const input = document.querySelector('input');"
                   "input.value='';"));
  waiter.WaitForNotification();

  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  ASSERT_EQ(0, n_characters);
  LONG caret_offset;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_caretOffset(&caret_offset));
  ASSERT_EQ(0, caret_offset);

  // Now that input is completely empty, the position of the caret should be
  // returned for character 0. The x,y position and height should be the same as
  // it was as when there was single character, but the width should now be 1.
  LONG x, y, width, height;
  for (int offset = IA2_TEXT_OFFSET_CARET; offset <= 0; ++offset) {
    EXPECT_HRESULT_SUCCEEDED(input_text->get_characterExtents(
        offset, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
    // TODO(accessibility) Why do results keep changing on each run?
    EXPECT_GE(prev_x + prev_width - 1, x);
    EXPECT_EQ(prev_y, y);
    EXPECT_EQ(1, width);
    EXPECT_EQ(prev_height, height);
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestCharacterExtentsWithAccessibilityModeChange) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraph(&paragraph_text, ui::AXMode::kNativeAPIs |
                                            ui::AXMode::kWebContents |
                                            ui::AXMode::kScreenReader);

  LONG x, y, width, height;
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(),
      ui::AXMode::kNativeAPIs | ui::AXMode::kWebContents |
          ui::AXMode::kScreenReader | ui::AXMode::kInlineTextBoxes,
      ax::mojom::Event::kLoadComplete);
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      0, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  // X and y coordinates should be available without
  // |ui::AXMode::kInlineTextBoxes|.
  EXPECT_LT(0, x);
  EXPECT_LT(0, y);
  // Width and height should be unavailable at this point.
  EXPECT_EQ(0, width);
  EXPECT_EQ(0, height);
  waiter.WaitForNotification();
  // Inline text boxes should have been enabled by this point.
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      0, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_LT(0, x);
  EXPECT_LT(0, y);
  EXPECT_LT(1, width);
  EXPECT_LT(1, height);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestScrollToPoint) {
  Microsoft::WRL::ComPtr<IAccessibleText> accessible_text;
  SetUpSampleParagraphInScrollableDocument(&accessible_text);
  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(accessible_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  LONG prev_x, prev_y, x, y, width, height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  ASSERT_HRESULT_SUCCEEDED(
      paragraph->accLocation(&prev_x, &prev_y, &width, &height, childid_self));
  AccessibilityNotificationWaiter location_changed_waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kLocationChanged);
  EXPECT_HRESULT_SUCCEEDED(
      paragraph->scrollToPoint(IA2_COORDTYPE_PARENT_RELATIVE, 0, 0));
  location_changed_waiter.WaitForNotification();

  ASSERT_HRESULT_SUCCEEDED(
      paragraph->accLocation(&x, &y, &width, &height, childid_self));
  EXPECT_EQ(prev_x, x);
  EXPECT_GT(prev_y, y);

  constexpr int kScrollToY = 0;
  EXPECT_HRESULT_SUCCEEDED(
      paragraph->scrollToPoint(IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToY));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(
      paragraph->accLocation(&x, &y, &width, &height, childid_self));
  EXPECT_EQ(kScrollToY, y);

  constexpr int kScrollToY_2 = 243;
  EXPECT_HRESULT_SUCCEEDED(
      paragraph->scrollToPoint(IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToY_2));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(
      paragraph->accLocation(&x, &y, &width, &height, childid_self));
  EXPECT_EQ(kScrollToY_2, y);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestScrollSubstringToPoint) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraphInScrollableDocument(&paragraph_text);
  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  LONG x, y, width, height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  AccessibilityNotificationWaiter location_changed_waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kLocationChanged);

  constexpr int kScrollToY = 187;
  constexpr int kCharOffset = 10;
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      kCharOffset, kCharOffset + 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0,
      kScrollToY));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      kCharOffset, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_EQ(kScrollToY, y);

  constexpr int kScrollToY_2 = -133;
  constexpr int kCharOffset_2 = 30;
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      kCharOffset_2, kCharOffset_2 + 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0,
      kScrollToY_2));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      kCharOffset_2, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_EQ(kScrollToY_2, y);
}

// https://crbug.com/948612
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestScrollVeryTallPaddingNearBottom) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpVeryTallPadding(&paragraph_text);
  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  LONG x, y, width, height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  AccessibilityNotificationWaiter location_changed_waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kLocationChanged);

  constexpr int kScrollToY = 500;
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      1, 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToY));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      1, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_EQ(kScrollToY, y);
}

// https://crbug.com/948612
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestScrollVeryTallPaddingNearTopOnce) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpVeryTallPadding(&paragraph_text);
  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  LONG x, y, width, height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  AccessibilityNotificationWaiter location_changed_waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kLocationChanged);

  constexpr int kScrollToY = 30;
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      1, 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToY));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      1, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_EQ(kScrollToY, y);
}

// https://crbug.com/948612
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestScrollVeryTallPaddingNearTopTwice) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpVeryTallPadding(&paragraph_text);
  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  LONG x, y, width, height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  AccessibilityNotificationWaiter location_changed_waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kLocationChanged);

  constexpr int kScrollToY = 30;
  constexpr int kScrollToYInitial = kScrollToY - 1;
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      1, 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToYInitial));
  location_changed_waiter.WaitForNotification();

  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      1, 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToY));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      1, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_EQ(kScrollToY, y);
}

// https://crbug.com/948612
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestScrollVeryTallMarginNearBottom) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpVeryTallMargin(&paragraph_text);
  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  LONG x, y, width, height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  AccessibilityNotificationWaiter location_changed_waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kLocationChanged);

  constexpr int kScrollToY = 500;
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      1, 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToY));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      1, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_EQ(kScrollToY, y);
}

// https://crbug.com/948612
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestScrollVeryTallMarginNearTopOnce) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpVeryTallMargin(&paragraph_text);
  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  LONG x, y, width, height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  AccessibilityNotificationWaiter location_changed_waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kLocationChanged);

  constexpr int kScrollToY = 30;
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      1, 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToY));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      1, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_EQ(kScrollToY, y);
}

// https://crbug.com/948612
IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestScrollVeryTallMarginNearTopTwice) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpVeryTallMargin(&paragraph_text);
  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  LONG x, y, width, height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  AccessibilityNotificationWaiter location_changed_waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kLocationChanged);

  constexpr int kScrollToY = 30;
  constexpr int kScrollToYInitial = kScrollToY - 1;
  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      1, 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToYInitial));
  location_changed_waiter.WaitForNotification();

  EXPECT_HRESULT_SUCCEEDED(paragraph_text->scrollSubstringToPoint(
      1, 1, IA2_COORDTYPE_SCREEN_RELATIVE, 0, kScrollToY));
  location_changed_waiter.WaitForNotification();
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_characterExtents(
      1, IA2_COORDTYPE_SCREEN_RELATIVE, &x, &y, &width, &height));
  EXPECT_EQ(kScrollToY, y);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestPutAccValueInInputField) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);

  Microsoft::WRL::ComPtr<IAccessible2> input;
  ASSERT_HRESULT_SUCCEEDED(input_text.CopyTo(IID_PPV_ARGS(&input)));

  base::win::ScopedVariant childid_self(CHILDID_SELF);
  base::win::ScopedBstr new_value(L"New value");
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kValueChanged);
  EXPECT_HRESULT_SUCCEEDED(input->put_accValue(childid_self, new_value));
  waiter.WaitForNotification();

  base::win::ScopedBstr value;
  EXPECT_HRESULT_SUCCEEDED(input->get_accValue(childid_self, value.Receive()));
  ASSERT_NE(nullptr, static_cast<BSTR>(value));
  EXPECT_STREQ(static_cast<BSTR>(new_value), static_cast<BSTR>(value));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestPutAccValueInTextarea) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);

  Microsoft::WRL::ComPtr<IAccessible2> textarea;
  ASSERT_HRESULT_SUCCEEDED(textarea_text.CopyTo(IID_PPV_ARGS(&textarea)));

  base::win::ScopedVariant childid_self(CHILDID_SELF);
  base::win::ScopedBstr new_value(L"New value");
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kValueChanged);
  EXPECT_HRESULT_SUCCEEDED(textarea->put_accValue(childid_self, new_value));
  waiter.WaitForNotification();

  base::win::ScopedBstr value;
  EXPECT_HRESULT_SUCCEEDED(
      textarea->get_accValue(childid_self, value.Receive()));
  ASSERT_NE(nullptr, static_cast<BSTR>(value));
  EXPECT_STREQ(static_cast<BSTR>(new_value), static_cast<BSTR>(value));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestPutAccValueInEditable) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraphInScrollableEditable(&paragraph_text);

  Microsoft::WRL::ComPtr<IAccessible2> paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&paragraph)));

  base::win::ScopedVariant childid_self(CHILDID_SELF);
  base::win::ScopedBstr new_value(L"New value");
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kValueChanged);
  EXPECT_HRESULT_SUCCEEDED(paragraph->put_accValue(childid_self, new_value));
  waiter.WaitForNotification();

  base::win::ScopedBstr value;
  EXPECT_HRESULT_SUCCEEDED(
      paragraph->get_accValue(childid_self, value.Receive()));
  ASSERT_NE(nullptr, static_cast<BSTR>(value));
  EXPECT_STREQ(static_cast<BSTR>(new_value), static_cast<BSTR>(value));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestSetCaretOffset) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);

  LONG caret_offset = 0;
  HRESULT hr = input_text->get_caretOffset(&caret_offset);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(static_cast<LONG>(InputContentsString().size() - 1), caret_offset);

  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  caret_offset = 0;
  hr = input_text->setCaretOffset(caret_offset);
  EXPECT_EQ(S_OK, hr);
  waiter.WaitForNotification();

  hr = input_text->get_caretOffset(&caret_offset);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(0, caret_offset);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestMultiLineSetCaretOffset) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);

  LONG caret_offset = 0;
  HRESULT hr = textarea_text->get_caretOffset(&caret_offset);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(static_cast<LONG>(InputContentsString().size() - 1), caret_offset);

  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  caret_offset = 0;
  hr = textarea_text->setCaretOffset(caret_offset);
  EXPECT_EQ(S_OK, hr);
  waiter.WaitForNotification();

  hr = textarea_text->get_caretOffset(&caret_offset);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(0, caret_offset);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestSetSelection) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);

  LONG start_offset, end_offset;
  EXPECT_HRESULT_FAILED(
      input_text->get_selection(1, &start_offset, &end_offset));
  HRESULT hr = input_text->get_selection(0, &start_offset, &end_offset);
  // There is no selection, just a caret.
  EXPECT_EQ(E_INVALIDARG, hr);

  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  int contents_string_length = int{InputContentsString().size()};
  start_offset = 0;
  end_offset = contents_string_length;
  EXPECT_HRESULT_FAILED(input_text->setSelection(1, start_offset, end_offset));
  EXPECT_HRESULT_SUCCEEDED(
      input_text->setSelection(0, start_offset, end_offset));
  waiter.WaitForNotification();

  hr = input_text->get_selection(0, &start_offset, &end_offset);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(contents_string_length, end_offset);

  start_offset = contents_string_length;
  end_offset = 1;
  EXPECT_HRESULT_SUCCEEDED(
      input_text->setSelection(0, start_offset, end_offset));
  waiter.WaitForNotification();

  hr = input_text->get_selection(0, &start_offset, &end_offset);
  EXPECT_EQ(S_OK, hr);
  // Start and end offsets are always swapped to be in ascending order.
  EXPECT_EQ(1, start_offset);
  EXPECT_EQ(contents_string_length, end_offset);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       DISABLED_TestSetSelectionRanges) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);
  Microsoft::WRL::ComPtr<IAccessible2_4> ax_input;
  ASSERT_HRESULT_SUCCEEDED(input_text.CopyTo(IID_PPV_ARGS(&ax_input)));

  LONG n_ranges = 1;
  IA2Range* ranges =
      reinterpret_cast<IA2Range*>(CoTaskMemAlloc(sizeof(IA2Range)));
  int contents_string_length = int{InputContentsString().size()};
  ranges[0].anchor = ax_input.Get();
  ranges[0].anchorOffset = -1;
  ranges[0].active = ax_input.Get();
  ranges[0].activeOffset = contents_string_length;
  EXPECT_HRESULT_FAILED(ax_input->setSelectionRanges(n_ranges, ranges));
  ranges[0].anchorOffset = 0;
  ranges[0].activeOffset = contents_string_length + 1;
  EXPECT_HRESULT_FAILED(ax_input->setSelectionRanges(n_ranges, ranges));

  ranges[0].activeOffset = contents_string_length;
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  EXPECT_HRESULT_SUCCEEDED(ax_input->setSelectionRanges(n_ranges, ranges));
  waiter.WaitForNotification();
  CoTaskMemFree(ranges);
  ranges = nullptr;
  n_ranges = 0;

  HRESULT hr = ax_input->get_selectionRanges(&ranges, &n_ranges);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(1, n_ranges);
  ASSERT_NE(nullptr, ranges);
  EXPECT_EQ(ax_input.Get(), ranges[0].anchor);
  EXPECT_EQ(0, ranges[0].anchorOffset);
  EXPECT_EQ(ax_input.Get(), ranges[0].active);
  EXPECT_EQ(contents_string_length, ranges[0].activeOffset);

  n_ranges = 1;
  ranges =
      reinterpret_cast<IA2Range*>(CoTaskMemRealloc(ranges, sizeof(IA2Range)));
  ranges[0].anchor = ax_input.Get();
  ranges[0].anchorOffset = contents_string_length;
  ranges[0].active = ax_input.Get();
  ranges[0].activeOffset = 1;
  EXPECT_HRESULT_SUCCEEDED(ax_input->setSelectionRanges(n_ranges, ranges));
  waiter.WaitForNotification();
  CoTaskMemFree(ranges);
  ranges = nullptr;
  n_ranges = 0;

  hr = ax_input->get_selectionRanges(&ranges, &n_ranges);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(1, n_ranges);
  ASSERT_NE(nullptr, ranges);
  EXPECT_EQ(ax_input.Get(), ranges[0].anchor);
  EXPECT_EQ(contents_string_length, ranges[0].anchorOffset);
  EXPECT_EQ(ax_input.Get(), ranges[0].active);
  EXPECT_EQ(1, ranges[0].activeOffset);
  CoTaskMemFree(ranges);
  ranges = nullptr;
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestMultiLineSetSelection) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);

  LONG start_offset, end_offset;
  EXPECT_HRESULT_FAILED(
      textarea_text->get_selection(1, &start_offset, &end_offset));
  HRESULT hr = textarea_text->get_selection(0, &start_offset, &end_offset);
  // There is no selection, just a caret.
  EXPECT_EQ(E_INVALIDARG, hr);

  int contents_string_length = int{InputContentsString().size()};
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  start_offset = 0;
  end_offset = contents_string_length;
  EXPECT_HRESULT_FAILED(
      textarea_text->setSelection(1, start_offset, end_offset));
  EXPECT_HRESULT_SUCCEEDED(
      textarea_text->setSelection(0, start_offset, end_offset));
  waiter.WaitForNotification();

  hr = textarea_text->get_selection(0, &start_offset, &end_offset);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(contents_string_length, end_offset);

  start_offset = contents_string_length - 1;
  end_offset = 0;
  EXPECT_HRESULT_SUCCEEDED(
      textarea_text->setSelection(0, start_offset, end_offset));
  waiter.WaitForNotification();

  hr = textarea_text->get_selection(0, &start_offset, &end_offset);
  EXPECT_EQ(S_OK, hr);
  // Start and end offsets are always swapped to be in ascending order.
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(contents_string_length - 1, end_offset);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       DISABLED_TestMultiLineSetSelectionRanges) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);
  Microsoft::WRL::ComPtr<IAccessible2_4> ax_textarea;
  ASSERT_HRESULT_SUCCEEDED(textarea_text.CopyTo(IID_PPV_ARGS(&ax_textarea)));

  int contents_string_length = int{InputContentsString().size()};
  LONG n_ranges = 1;
  IA2Range* ranges =
      reinterpret_cast<IA2Range*>(CoTaskMemAlloc(sizeof(IA2Range)));
  ranges[0].anchor = ax_textarea.Get();
  ranges[0].anchorOffset = -1;
  ranges[0].active = ax_textarea.Get();
  ranges[0].activeOffset = contents_string_length;
  EXPECT_HRESULT_FAILED(ax_textarea->setSelectionRanges(n_ranges, ranges));
  ranges[0].anchorOffset = 0;
  ranges[0].activeOffset = contents_string_length + 1;
  EXPECT_HRESULT_FAILED(ax_textarea->setSelectionRanges(n_ranges, ranges));

  ranges[0].activeOffset = contents_string_length;
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kTextSelectionChanged);
  EXPECT_HRESULT_SUCCEEDED(ax_textarea->setSelectionRanges(n_ranges, ranges));
  waiter.WaitForNotification();
  CoTaskMemFree(ranges);
  ranges = nullptr;
  n_ranges = 0;

  HRESULT hr = ax_textarea->get_selectionRanges(&ranges, &n_ranges);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(1, n_ranges);
  ASSERT_NE(nullptr, ranges);
  EXPECT_EQ(ax_textarea.Get(), ranges[0].anchor);
  EXPECT_EQ(0, ranges[0].anchorOffset);
  EXPECT_EQ(ax_textarea.Get(), ranges[0].active);
  EXPECT_EQ(contents_string_length, ranges[0].activeOffset);

  n_ranges = 1;
  ranges =
      reinterpret_cast<IA2Range*>(CoTaskMemRealloc(ranges, sizeof(IA2Range)));
  ranges[0].anchor = ax_textarea.Get();
  ranges[0].anchorOffset = contents_string_length - 1;
  ranges[0].active = ax_textarea.Get();
  ranges[0].activeOffset = 0;
  EXPECT_HRESULT_SUCCEEDED(ax_textarea->setSelectionRanges(n_ranges, ranges));
  waiter.WaitForNotification();
  CoTaskMemFree(ranges);
  ranges = nullptr;
  n_ranges = 0;

  hr = ax_textarea->get_selectionRanges(&ranges, &n_ranges);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(1, n_ranges);
  ASSERT_NE(nullptr, ranges);
  EXPECT_EQ(ax_textarea.Get(), ranges[0].anchor);
  EXPECT_EQ(contents_string_length - 1, ranges[0].anchorOffset);
  EXPECT_EQ(ax_textarea.Get(), ranges[0].active);
  EXPECT_EQ(0, ranges[0].activeOffset);
  CoTaskMemFree(ranges);
  ranges = nullptr;
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestStaticTextSetSelection) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraph(&paragraph_text);

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_nCharacters(&n_characters));
  ASSERT_LT(0, n_characters);

  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kDocumentSelectionChanged);
  LONG start_offset = 0;
  LONG end_offset = n_characters;
  EXPECT_HRESULT_FAILED(
      paragraph_text->setSelection(1, start_offset, end_offset));
  EXPECT_HRESULT_SUCCEEDED(
      paragraph_text->setSelection(0, start_offset, end_offset));
  waiter.WaitForNotification();

  HRESULT hr = paragraph_text->get_selection(0, &start_offset, &end_offset);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(n_characters, end_offset);

  start_offset = n_characters - 1;
  end_offset = 0;
  EXPECT_HRESULT_SUCCEEDED(
      paragraph_text->setSelection(0, start_offset, end_offset));
  waiter.WaitForNotification();

  hr = paragraph_text->get_selection(0, &start_offset, &end_offset);
  EXPECT_EQ(S_OK, hr);
  // Start and end offsets are always swapped to be in ascending order.
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(n_characters - 1, end_offset);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       DISABLED_TestStaticTextSetSelectionRanges) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraph(&paragraph_text);
  Microsoft::WRL::ComPtr<IAccessible2_4> ax_paragraph;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text.CopyTo(IID_PPV_ARGS(&ax_paragraph)));

  LONG child_count = 0;
  ASSERT_HRESULT_SUCCEEDED(ax_paragraph->get_accChildCount(&child_count));
  ASSERT_LT(0, child_count);

  LONG n_ranges = 1;
  IA2Range* ranges =
      reinterpret_cast<IA2Range*>(CoTaskMemAlloc(sizeof(IA2Range)));
  ranges[0].anchor = ax_paragraph.Get();
  ranges[0].anchorOffset = -1;
  ranges[0].active = ax_paragraph.Get();
  ranges[0].activeOffset = child_count;
  EXPECT_HRESULT_FAILED(ax_paragraph->setSelectionRanges(n_ranges, ranges));
  ranges[0].anchorOffset = 0;
  ranges[0].activeOffset = child_count + 1;
  EXPECT_HRESULT_FAILED(ax_paragraph->setSelectionRanges(n_ranges, ranges));

  ranges[0].activeOffset = child_count;
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kDocumentSelectionChanged);
  EXPECT_HRESULT_SUCCEEDED(ax_paragraph->setSelectionRanges(n_ranges, ranges));
  waiter.WaitForNotification();
  CoTaskMemFree(ranges);
  ranges = nullptr;
  n_ranges = 0;

  HRESULT hr = ax_paragraph->get_selectionRanges(&ranges, &n_ranges);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(1, n_ranges);
  ASSERT_NE(nullptr, ranges);
  EXPECT_EQ(ax_paragraph.Get(), ranges[0].anchor);
  EXPECT_EQ(0, ranges[0].anchorOffset);
  EXPECT_EQ(ax_paragraph.Get(), ranges[0].active);
  EXPECT_EQ(child_count, ranges[0].activeOffset);

  n_ranges = 1;
  ranges =
      reinterpret_cast<IA2Range*>(CoTaskMemRealloc(ranges, sizeof(IA2Range)));
  ranges[0].anchor = ax_paragraph.Get();
  ranges[0].anchorOffset = child_count - 1;
  ranges[0].active = ax_paragraph.Get();
  ranges[0].activeOffset = 0;
  EXPECT_HRESULT_SUCCEEDED(ax_paragraph->setSelectionRanges(n_ranges, ranges));
  waiter.WaitForNotification();
  CoTaskMemFree(ranges);
  ranges = nullptr;
  n_ranges = 0;

  hr = ax_paragraph->get_selectionRanges(&ranges, &n_ranges);
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(1, n_ranges);
  ASSERT_NE(nullptr, ranges);
  EXPECT_EQ(ax_paragraph.Get(), ranges[0].anchor);
  EXPECT_EQ(static_cast<LONG>(InputContentsString().size() - 1),
            ranges[0].anchorOffset);
  EXPECT_EQ(ax_paragraph.Get(), ranges[0].active);
  EXPECT_EQ(0, ranges[0].activeOffset);
  CoTaskMemFree(ranges);
  ranges = nullptr;
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestTextAtOffsetWithInvalidArguments) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);
  HRESULT hr =
      input_text->get_textAtOffset(0, IA2_TEXT_BOUNDARY_CHAR, NULL, NULL, NULL);
  EXPECT_EQ(E_INVALIDARG, hr);

  // Test invalid offset.
  LONG start_offset = 0;
  LONG end_offset = 0;
  base::win::ScopedBstr text;
  LONG invalid_offset = -5;
  hr = input_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_CHAR,
                                    &start_offset, &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  invalid_offset = InputContentsString().size() + 1;
  hr = input_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_WORD,
                                    &start_offset, &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));

  // According to the IA2 Spec, only line boundaries should succeed when
  // the offset is one past the end of the text.
  invalid_offset = InputContentsString().size();
  hr = input_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_CHAR,
                                    &start_offset, &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = input_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_WORD,
                                    &start_offset, &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = input_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_SENTENCE,
                                    &start_offset, &end_offset, text.Receive());
  EXPECT_EQ(S_FALSE, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = input_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_LINE,
                                    &start_offset, &end_offset, text.Receive());
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(46, end_offset);
  EXPECT_STREQ(L"Moz/5.0 (ST 6.x; WWW33) WebKit  \"KHTML, like\".", text);
  text.Reset();
  hr = input_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_ALL,
                                    &start_offset, &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));

  // The same behavior should be observed when the special offset
  // IA2_TEXT_OFFSET_LENGTH is used.
  hr = input_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                    IA2_TEXT_BOUNDARY_CHAR, &start_offset,
                                    &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = input_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                    IA2_TEXT_BOUNDARY_WORD, &start_offset,
                                    &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = input_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                    IA2_TEXT_BOUNDARY_SENTENCE, &start_offset,
                                    &end_offset, text.Receive());
  EXPECT_EQ(S_FALSE, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = input_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                    IA2_TEXT_BOUNDARY_LINE, &start_offset,
                                    &end_offset, text.Receive());
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(46, end_offset);
  EXPECT_STREQ(L"Moz/5.0 (ST 6.x; WWW33) WebKit  \"KHTML, like\".", text);
  text.Reset();
  hr = input_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                    IA2_TEXT_BOUNDARY_ALL, &start_offset,
                                    &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestMultiLineTextAtOffsetWithInvalidArguments) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);
  HRESULT hr = textarea_text->get_textAtOffset(0, IA2_TEXT_BOUNDARY_CHAR, NULL,
                                               NULL, NULL);
  EXPECT_EQ(E_INVALIDARG, hr);

  // Test invalid offset.
  LONG start_offset = 0;
  LONG end_offset = 0;
  base::win::ScopedBstr text;
  LONG invalid_offset = -5;
  hr = textarea_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_CHAR,
                                       &start_offset, &end_offset,
                                       text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  invalid_offset = InputContentsString().size() + 1;
  hr = textarea_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_WORD,
                                       &start_offset, &end_offset,
                                       text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));

  // According to the IA2 Spec, only line boundaries should succeed when
  // the offset is one past the end of the text.
  invalid_offset = InputContentsString().size();
  hr = textarea_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_CHAR,
                                       &start_offset, &end_offset,
                                       text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = textarea_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_WORD,
                                       &start_offset, &end_offset,
                                       text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = textarea_text->get_textAtOffset(
      invalid_offset, IA2_TEXT_BOUNDARY_SENTENCE, &start_offset, &end_offset,
      text.Receive());
  EXPECT_EQ(S_FALSE, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = textarea_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_LINE,
                                       &start_offset, &end_offset,
                                       text.Receive());
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(32, start_offset);
  EXPECT_EQ(46, end_offset);
  EXPECT_STREQ(L"\"KHTML, like\".", text);
  text.Reset();
  hr = textarea_text->get_textAtOffset(invalid_offset, IA2_TEXT_BOUNDARY_ALL,
                                       &start_offset, &end_offset,
                                       text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));

  // The same behavior should be observed when the special offset
  // IA2_TEXT_OFFSET_LENGTH is used.
  hr = textarea_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                       IA2_TEXT_BOUNDARY_CHAR, &start_offset,
                                       &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = textarea_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                       IA2_TEXT_BOUNDARY_WORD, &start_offset,
                                       &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = textarea_text->get_textAtOffset(
      IA2_TEXT_OFFSET_LENGTH, IA2_TEXT_BOUNDARY_SENTENCE, &start_offset,
      &end_offset, text.Receive());
  EXPECT_EQ(S_FALSE, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
  hr = textarea_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                       IA2_TEXT_BOUNDARY_LINE, &start_offset,
                                       &end_offset, text.Receive());
  EXPECT_EQ(S_OK, hr);
  EXPECT_EQ(32, start_offset);
  EXPECT_EQ(46, end_offset);
  EXPECT_STREQ(L"\"KHTML, like\".", text);
  text.Reset();
  hr = textarea_text->get_textAtOffset(IA2_TEXT_OFFSET_LENGTH,
                                       IA2_TEXT_BOUNDARY_ALL, &start_offset,
                                       &end_offset, text.Receive());
  EXPECT_EQ(E_INVALIDARG, hr);
  EXPECT_EQ(0, start_offset);
  EXPECT_EQ(0, end_offset);
  EXPECT_EQ(nullptr, static_cast<BSTR>(text));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestTextAtOffsetWithBoundaryCharacter) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);

  int contents_string_length = int{InputContentsString().size()};
  for (LONG offset = 0; offset < contents_string_length; ++offset) {
    std::wstring expected_text(1, InputContentsString()[offset]);
    LONG expected_start_offset = offset;
    LONG expected_end_offset = offset + 1;
    CheckTextAtOffset(input_text, offset, IA2_TEXT_BOUNDARY_CHAR,
                      expected_start_offset, expected_end_offset,
                      expected_text);
  }

  for (LONG offset = contents_string_length - 1; offset >= 0; --offset) {
    std::wstring expected_text(1, InputContentsString()[offset]);
    LONG expected_start_offset = offset;
    LONG expected_end_offset = offset + 1;
    CheckTextAtOffset(input_text, offset, IA2_TEXT_BOUNDARY_CHAR,
                      expected_start_offset, expected_end_offset,
                      expected_text);
  }

  CheckTextAtOffset(input_text, IA2_TEXT_OFFSET_CARET, IA2_TEXT_BOUNDARY_CHAR,
                    contents_string_length - 1, contents_string_length, L".");
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestMultiLineTextAtOffsetWithBoundaryCharacter) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);

  int contents_string_length = InputContentsString().size();
  for (LONG offset = 0; offset < contents_string_length; ++offset) {
    std::wstring expected_text(1, TextAreaContentsString()[offset]);
    LONG expected_start_offset = offset;
    LONG expected_end_offset = offset + 1;
    CheckTextAtOffset(textarea_text, offset, IA2_TEXT_BOUNDARY_CHAR,
                      expected_start_offset, expected_end_offset,
                      expected_text);
  }

  for (LONG offset = contents_string_length - 1; offset >= 0; --offset) {
    std::wstring expected_text(1, TextAreaContentsString()[offset]);
    LONG expected_start_offset = offset;
    LONG expected_end_offset = offset + 1;
    CheckTextAtOffset(textarea_text, offset, IA2_TEXT_BOUNDARY_CHAR,
                      expected_start_offset, expected_end_offset,
                      expected_text);
  }

  CheckTextAtOffset(textarea_text, IA2_TEXT_OFFSET_CARET,
                    IA2_TEXT_BOUNDARY_CHAR, contents_string_length - 1,
                    contents_string_length, L".");
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestMultilingualTextAtOffsetWithBoundaryCharacter) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kValueChanged);
  // Place an e acute, and two emoticons in the text field.
  ExecuteScript(base::UTF8ToUTF16(R"SCRIPT(
      const input = document.querySelector('input');
      input.value =
          'e\u0301\uD83D\uDC69\u200D\u2764\uFE0F\u200D\uD83D\uDC69\uD83D\uDC36';
      )SCRIPT"));
  waiter.WaitForNotification();

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(input_text->get_nCharacters(&n_characters));
  // "n_characters" is the number of valid text offsets.
  //
  // Ordinarily, the number of valid text offsets should equal the number of
  // actual characters which are only three in this case. However, this is
  // harder to implement given our current UTF16-based representation of IA2
  // hyptertext.
  // TODO(nektar): Implement support for base::OffsetAdjuster in AXPosition.
  ASSERT_EQ(12, n_characters);

  // The expected text consists of an e acute, and two emoticons.
  const std::vector<std::wstring> expected_text = {
      L"e\x0301", L"\xD83D\xDC69\x200D\x2764\xFE0F\x200D\xD83D\xDC69",
      L"\xD83D\xDC36"};
  LONG offset = 0;
  for (const std::wstring& expected_character : expected_text) {
    LONG expected_start_offset = offset;
    LONG expected_end_offset =
        expected_start_offset + LONG{expected_character.length()};
    for (size_t code_unit_offset = 0;
         code_unit_offset < expected_character.length(); ++code_unit_offset) {
      CheckTextAtOffset(input_text, offset, IA2_TEXT_BOUNDARY_CHAR,
                        expected_start_offset, expected_end_offset,
                        expected_character);
      ++offset;
    }
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestTextAtOffsetWithBoundaryWord) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);

  // Trailing punctuation should be included as part of the previous word.
  CheckTextAtOffset(input_text, 0, IA2_TEXT_BOUNDARY_WORD, 0, 4, L"Moz/");
  CheckTextAtOffset(input_text, 2, IA2_TEXT_BOUNDARY_WORD, 0, 4, L"Moz/");

  // If the offset is at the punctuation, it should return
  // the previous word.
  CheckTextAtOffset(input_text, 3, IA2_TEXT_BOUNDARY_WORD, 0, 4, L"Moz/");

  // Numbers with a decimal point ("." for U.S), should be treated as one word.
  // Also, trailing punctuation that occurs after empty space should be part of
  // the word. ("5.0 (" and not "5.0 ".)
  CheckTextAtOffset(input_text, 4, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");
  CheckTextAtOffset(input_text, 5, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");
  CheckTextAtOffset(input_text, 6, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");
  CheckTextAtOffset(input_text, 7, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");

  // Leading punctuation should not be included with the word after it.
  CheckTextAtOffset(input_text, 8, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");
  CheckTextAtOffset(input_text, 11, IA2_TEXT_BOUNDARY_WORD, 9, 12, L"ST ");

  // Numbers separated from letters with trailing punctuation should
  // be split into two words. Same for abreviations like "i.e.".
  CheckTextAtOffset(input_text, 12, IA2_TEXT_BOUNDARY_WORD, 12, 14, L"6.");
  CheckTextAtOffset(input_text, 15, IA2_TEXT_BOUNDARY_WORD, 14, 17, L"x; ");

  // Words with numbers should be treated like ordinary words.
  CheckTextAtOffset(input_text, 17, IA2_TEXT_BOUNDARY_WORD, 17, 24, L"WWW33) ");
  CheckTextAtOffset(input_text, 23, IA2_TEXT_BOUNDARY_WORD, 17, 24, L"WWW33) ");

  // Multiple trailing empty spaces should be part of the word preceding it.
  CheckTextAtOffset(input_text, 28, IA2_TEXT_BOUNDARY_WORD, 24, 33,
                    L"WebKit  \"");
  CheckTextAtOffset(input_text, 31, IA2_TEXT_BOUNDARY_WORD, 24, 33,
                    L"WebKit  \"");
  CheckTextAtOffset(input_text, 32, IA2_TEXT_BOUNDARY_WORD, 24, 33,
                    L"WebKit  \"");

  // Leading punctuation such as quotation marks should not be part of the word.
  CheckTextAtOffset(input_text, 33, IA2_TEXT_BOUNDARY_WORD, 33, 40, L"KHTML, ");
  CheckTextAtOffset(input_text, 38, IA2_TEXT_BOUNDARY_WORD, 33, 40, L"KHTML, ");

  // Trailing final punctuation should be part of the last word.
  int contents_string_length = int{InputContentsString().size()};
  CheckTextAtOffset(input_text, 41, IA2_TEXT_BOUNDARY_WORD, 40,
                    contents_string_length, L"like\".");
  CheckTextAtOffset(input_text, 45, IA2_TEXT_BOUNDARY_WORD, 40,
                    contents_string_length, L"like\".");

  // Test special offsets.
  CheckTextAtOffset(input_text, IA2_TEXT_OFFSET_CARET, IA2_TEXT_BOUNDARY_WORD,
                    40, contents_string_length, L"like\".");
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestMultiLineTextAtOffsetWithBoundaryWord) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);

  // Trailing punctuation should be included as part of the previous word.
  CheckTextAtOffset(textarea_text, 0, IA2_TEXT_BOUNDARY_WORD, 0, 4, L"Moz/");
  CheckTextAtOffset(textarea_text, 2, IA2_TEXT_BOUNDARY_WORD, 0, 4, L"Moz/");

  // If the offset is at the punctuation, it should return
  // the previous word.
  CheckTextAtOffset(textarea_text, 3, IA2_TEXT_BOUNDARY_WORD, 0, 4, L"Moz/");

  // Numbers with a decimal point ("." for U.S), should be treated as one word.
  // Also, trailing punctuation that occurs after empty space should be part of
  // the word. ("5.0 (" and not "5.0 ".)
  CheckTextAtOffset(textarea_text, 4, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");
  CheckTextAtOffset(textarea_text, 5, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");
  CheckTextAtOffset(textarea_text, 6, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");
  CheckTextAtOffset(textarea_text, 7, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");

  // Leading punctuation should not be included with the word after it.
  CheckTextAtOffset(textarea_text, 8, IA2_TEXT_BOUNDARY_WORD, 4, 9, L"5.0 (");
  CheckTextAtOffset(textarea_text, 11, IA2_TEXT_BOUNDARY_WORD, 9, 12, L"ST ");

  // Numbers separated from letters with trailing punctuation should
  // be split into two words. Same for abreviations like "i.e.".
  CheckTextAtOffset(textarea_text, 12, IA2_TEXT_BOUNDARY_WORD, 12, 14, L"6.");
  CheckTextAtOffset(textarea_text, 15, IA2_TEXT_BOUNDARY_WORD, 14, 17, L"x; ");

  // Words with numbers should be treated like ordinary words.
  CheckTextAtOffset(textarea_text, 17, IA2_TEXT_BOUNDARY_WORD, 17, 24,
                    L"WWW33)\n");
  CheckTextAtOffset(textarea_text, 23, IA2_TEXT_BOUNDARY_WORD, 17, 24,
                    L"WWW33)\n");

  // Multiple trailing empty spaces should be part of the word preceding it.
  CheckTextAtOffset(textarea_text, 28, IA2_TEXT_BOUNDARY_WORD, 24, 33,
                    L"WebKit \n\"");
  CheckTextAtOffset(textarea_text, 31, IA2_TEXT_BOUNDARY_WORD, 24, 33,
                    L"WebKit \n\"");
  CheckTextAtOffset(textarea_text, 32, IA2_TEXT_BOUNDARY_WORD, 24, 33,
                    L"WebKit \n\"");

  // Leading punctuation such as quotation marks should not be part of the word.
  CheckTextAtOffset(textarea_text, 33, IA2_TEXT_BOUNDARY_WORD, 33, 40,
                    L"KHTML, ");
  CheckTextAtOffset(textarea_text, 38, IA2_TEXT_BOUNDARY_WORD, 33, 40,
                    L"KHTML, ");

  // Trailing final punctuation should be part of the last word.
  int contents_string_length = int{InputContentsString().size()};
  CheckTextAtOffset(textarea_text, 41, IA2_TEXT_BOUNDARY_WORD, 40,
                    contents_string_length, L"like\".");
  CheckTextAtOffset(textarea_text, 45, IA2_TEXT_BOUNDARY_WORD, 40,
                    contents_string_length, L"like\".");

  // Test special offsets.
  CheckTextAtOffset(textarea_text, IA2_TEXT_OFFSET_CARET,
                    IA2_TEXT_BOUNDARY_WORD, 40, contents_string_length,
                    L"like\".");
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestStaticTextAtOffsetWithBoundaryWord) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraph(&paragraph_text);
  base::string16 embedded_character(
      1, BrowserAccessibilityComWin::kEmbeddedCharacter);
  std::vector<std::wstring> words;
  words.push_back(L"Game ");
  words.push_back(L"theory ");
  words.push_back(L"is \"");
  words.push_back(L"the ");
  words.push_back(L"study ");
  words.push_back(L"of ");
  words.push_back(embedded_character);
  words.push_back(L"of ");
  words.push_back(L"conflict ");
  words.push_back(L"and\n");
  words.push_back(L"cooperation ");
  words.push_back(L"between ");
  words.push_back(L"intelligent ");
  words.push_back(L"rational ");
  words.push_back(L"decision-");
  words.push_back(L"makers.\"");

  // Try to retrieve one word after another.
  LONG word_start_offset = 0;
  for (auto& word : words) {
    LONG word_end_offset = word_start_offset + word.size();
    CheckTextAtOffset(paragraph_text, word_start_offset, IA2_TEXT_BOUNDARY_WORD,
                      word_start_offset, word_end_offset, word);
    word_start_offset = word_end_offset;
    // If the word boundary is inside an embedded object, |word_end_offset|
    // should be one past the embedded object character. To get to the start of
    // the next word, we have to skip the space between the embedded object
    // character and the next word.
    if (word == embedded_character)
      ++word_start_offset;
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestTextAtOffsetWithBoundarySentence) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);

  // Sentence navigation is not currently implemented.
  LONG start_offset = 0;
  LONG end_offset = 0;
  base::win::ScopedBstr text;
  HRESULT hr =
      input_text->get_textAtOffset(5, IA2_TEXT_BOUNDARY_SENTENCE, &start_offset,
                                   &end_offset, text.Receive());
  EXPECT_EQ(S_FALSE, hr);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestMultiLineTextAtOffsetWithBoundarySentence) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);

  // Sentence navigation is not currently implemented.
  LONG start_offset = 0;
  LONG end_offset = 0;
  base::win::ScopedBstr text;
  HRESULT hr = textarea_text->get_textAtOffset(25, IA2_TEXT_BOUNDARY_SENTENCE,
                                               &start_offset, &end_offset,
                                               text.Receive());
  EXPECT_EQ(S_FALSE, hr);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestTextAtOffsetWithBoundaryLine) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);

  // Single line text fields should return the whole text.
  int contents_string_length = int{InputContentsString().size()};
  CheckTextAtOffset(input_text, 0, IA2_TEXT_BOUNDARY_LINE, 0,
                    contents_string_length,
                    base::SysUTF8ToWide(InputContentsString()));

  // Test special offsets.
  CheckTextAtOffset(input_text, IA2_TEXT_OFFSET_LENGTH, IA2_TEXT_BOUNDARY_LINE,
                    0, contents_string_length,
                    base::SysUTF8ToWide(InputContentsString()));
  CheckTextAtOffset(input_text, IA2_TEXT_OFFSET_CARET, IA2_TEXT_BOUNDARY_LINE,
                    0, contents_string_length,
                    base::SysUTF8ToWide(InputContentsString()));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestMultiLineTextAtOffsetWithBoundaryLine) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);

  CheckTextAtOffset(textarea_text, 0, IA2_TEXT_BOUNDARY_LINE, 0, 24,
                    L"Moz/5.0 (ST 6.x; WWW33)\n");

  // If the offset is at the newline, return the line preceding it.
  CheckTextAtOffset(textarea_text, 31, IA2_TEXT_BOUNDARY_LINE, 24, 32,
                    L"WebKit \n");

  // Last line does not have a trailing newline.
  int contents_string_length = int{InputContentsString().size()};
  CheckTextAtOffset(textarea_text, 32, IA2_TEXT_BOUNDARY_LINE, 32,
                    contents_string_length, L"\"KHTML, like\".");

  // An offset one past the last character should return the last line.
  CheckTextAtOffset(textarea_text, contents_string_length,
                    IA2_TEXT_BOUNDARY_LINE, 32, contents_string_length,
                    L"\"KHTML, like\".");

  // Test special offsets.
  CheckTextAtOffset(textarea_text, IA2_TEXT_OFFSET_LENGTH,
                    IA2_TEXT_BOUNDARY_LINE, 32, contents_string_length,
                    L"\"KHTML, like\".");
  CheckTextAtOffset(textarea_text, IA2_TEXT_OFFSET_CARET,
                    IA2_TEXT_BOUNDARY_LINE, 32, contents_string_length,
                    L"\"KHTML, like\".");
}

IN_PROC_BROWSER_TEST_F(
    AccessibilityWinBrowserTest,
    TestTextAtOffsetWithBoundaryLineAndMultiLineEmbeddedObject) {
  // There should be two lines in this contenteditable.
  //
  // Half of the link is on the first line, and the other half is on the second
  // line.
  LoadInitialAccessibilityTreeFromHtml(R"HTML(<!DOCTYPE html>
      <div contenteditable style="width: 70px">
        Hello
        <a href="#">this is a</a>
        test.
      </div>
      )HTML");

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> contenteditable;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      &contenteditable));

  Microsoft::WRL::ComPtr<IAccessibleText> contenteditable_text;
  ASSERT_HRESULT_SUCCEEDED(contenteditable.As(&contenteditable_text));

  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(
      contenteditable_text->get_nCharacters(&n_characters));
  ASSERT_EQ(13, n_characters);

  // Line one.
  //
  // The embedded object character representing the link is at offset 6.
  for (LONG i = 0; i <= 6; ++i) {
    CheckTextAtOffset(contenteditable_text, i, IA2_TEXT_BOUNDARY_LINE, 0, 7,
                      L"Hello \xFFFC");
  }

  // Line two.
  //
  // Note that the caret can also be at the end of the contenteditable, so an
  // offset that is equal to "n_characters" is also permitted.
  for (LONG i = 7; i <= n_characters; ++i) {
    CheckTextAtOffset(contenteditable_text, i, IA2_TEXT_BOUNDARY_LINE, 6,
                      n_characters, L"\xFFFC test.");
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestParagraphTextAtOffsetWithBoundaryLine) {
  Microsoft::WRL::ComPtr<IAccessibleText> paragraph_text;
  SetUpSampleParagraph(&paragraph_text);

  // There should be two lines in this paragraph.
  const LONG newline_offset = 46;
  LONG n_characters;
  ASSERT_HRESULT_SUCCEEDED(paragraph_text->get_nCharacters(&n_characters));
  ASSERT_LT(0, n_characters);
  ASSERT_LT(newline_offset, n_characters);

  for (LONG i = 0; i <= newline_offset; ++i) {
    CheckTextAtOffset(
        paragraph_text, i, IA2_TEXT_BOUNDARY_LINE, 0, newline_offset + 1,
        L"Game theory is \"the study of \xFFFC of conflict and\n");
  }

  // For line boundaries, IA2 Spec allows for the offset to be equal to the
  // text's length.
  for (LONG i = newline_offset + 1; i <= n_characters; ++i) {
    CheckTextAtOffset(
        paragraph_text, i, IA2_TEXT_BOUNDARY_LINE, newline_offset + 1,
        n_characters,
        L"cooperation between intelligent rational decision-makers.\"");
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestTextAtOffsetWithBoundaryAll) {
  Microsoft::WRL::ComPtr<IAccessibleText> input_text;
  SetUpInputField(&input_text);

  CheckTextAtOffset(input_text, 0, IA2_TEXT_BOUNDARY_ALL, 0,
                    InputContentsString().size(),
                    base::SysUTF8ToWide(InputContentsString()));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestMultiLineTextAtOffsetWithBoundaryAll) {
  Microsoft::WRL::ComPtr<IAccessibleText> textarea_text;
  SetUpTextareaField(&textarea_text);

  CheckTextAtOffset(textarea_text, InputContentsString().size() - 1,
                    IA2_TEXT_BOUNDARY_ALL, 0, InputContentsString().size(),
                    base::SysUTF8ToWide(TextAreaContentsString()));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestIAccessibleAction) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
      <html>
      <body>
        <img src="" alt="image"
            onclick="document.querySelector('img').alt = 'image2';">
      </body>
      </html>)HTML");

  // Retrieve the IAccessible interface for the web page.
  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> div;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      div.GetAddressOf()));
  std::vector<base::win::ScopedVariant> div_children =
      GetAllAccessibleChildren(div.Get());
  ASSERT_EQ(1u, div_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> image;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(div.Get(), div_children[0].AsInput()).Get(),
      image.GetAddressOf()));
  LONG image_role = 0;
  ASSERT_HRESULT_SUCCEEDED(image->role(&image_role));
  ASSERT_EQ(ROLE_SYSTEM_GRAPHIC, image_role);

  Microsoft::WRL::ComPtr<IAccessibleAction> image_action;
  ASSERT_HRESULT_SUCCEEDED(image.CopyTo(image_action.GetAddressOf()));

  LONG n_actions = 0;
  EXPECT_HRESULT_SUCCEEDED(image_action->nActions(&n_actions));
  EXPECT_EQ(1, n_actions);

  base::win::ScopedBstr action_name;
  EXPECT_HRESULT_SUCCEEDED(image_action->get_name(0, action_name.Receive()));
  EXPECT_EQ(L"click", std::wstring(action_name, action_name.Length()));
  action_name.Release();
  EXPECT_HRESULT_FAILED(image_action->get_name(1, action_name.Receive()));
  EXPECT_EQ(nullptr, static_cast<BSTR>(action_name));

  base::win::ScopedVariant childid_self(CHILDID_SELF);
  base::win::ScopedBstr image_name;
  EXPECT_HRESULT_SUCCEEDED(
      image->get_accName(childid_self, image_name.Receive()));
  EXPECT_EQ(L"image", std::wstring(image_name, image_name.Length()));
  image_name.Release();
  // Cllicking the image will change its name.
  EXPECT_HRESULT_SUCCEEDED(image_action->doAction(0));
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kTextChanged);
  waiter.WaitForNotification();
  EXPECT_HRESULT_SUCCEEDED(
      image->get_accName(childid_self, image_name.Receive()));
  EXPECT_EQ(L"image2", std::wstring(image_name, image_name.Length()));
  EXPECT_HRESULT_FAILED(image_action->doAction(1));
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, HasHWNDAfterNavigation) {
  // This test simulates a scenario where RenderWidgetHostViewAura::SetSize
  // is not called again after its window is added to the root window.
  // Ensure that we still get a legacy HWND for accessibility.
  ASSERT_TRUE(embedded_test_server()->Start());
  WebContentsImpl* web_contents =
      static_cast<WebContentsImpl*>(shell()->web_contents());
  WebContentsView* web_contents_view = web_contents->GetView();
  WebContentsViewAura* web_contents_view_aura =
      static_cast<WebContentsViewAura*>(web_contents_view);

  // Set a flag that will cause WebContentsViewAura to initialize a
  // RenderWidgetHostViewAura with a null parent view.
  web_contents_view_aura->set_init_rwhv_with_null_parent_for_testing(true);

  // Enable accessibility.
  AccessibilityNotificationWaiter waiter(web_contents, ui::kAXModeComplete,
                                         ax::mojom::Event::kLoadComplete);

  // Navigate to a new page.
  EXPECT_TRUE(NavigateToURL(shell(), embedded_test_server()->GetURL(
                                         "/accessibility/html/article.html")));

  // At this point the root of the accessibility tree shouldn't have an HWND
  // because we never gave a parent window to the RWHVA.
  BrowserAccessibilityManagerWin* manager =
      static_cast<BrowserAccessibilityManagerWin*>(
          web_contents->GetRootBrowserAccessibilityManager());
  ASSERT_EQ(nullptr, manager->GetParentHWND());

  // Now add the RWHVA's window to the root window and ensure that we have
  // an HWND for accessibility now.
  web_contents_view->GetNativeView()->AddChild(
      web_contents->GetRenderWidgetHostView()->GetNativeView());
  // The load event will only fire after the page is attached.
  waiter.WaitForNotification();
  ASSERT_NE(nullptr, manager->GetParentHWND());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestAccNavigateInTables) {
  ASSERT_TRUE(embedded_test_server()->Start());
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kLoadComplete);
  EXPECT_TRUE(NavigateToURL(
      shell(),
      embedded_test_server()->GetURL("/accessibility/html/table-spans.html")));
  waiter.WaitForNotification();

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  // There are two tables in this test file. Use only the first one.
  ASSERT_EQ(2u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> table;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      table.GetAddressOf()));
  LONG role = 0;
  ASSERT_HRESULT_SUCCEEDED(table->role(&role));
  ASSERT_EQ(ROLE_SYSTEM_TABLE, role);

  // Retrieve the first cell.
  Microsoft::WRL::ComPtr<IAccessibleTable2> table2;
  Microsoft::WRL::ComPtr<IUnknown> cell;
  Microsoft::WRL::ComPtr<IAccessible2> cell1;
  EXPECT_HRESULT_SUCCEEDED(table.CopyTo(table2.GetAddressOf()));
  EXPECT_HRESULT_SUCCEEDED(table2->get_cellAt(0, 0, cell.GetAddressOf()));
  EXPECT_HRESULT_SUCCEEDED(cell.CopyTo(cell1.GetAddressOf()));

  base::win::ScopedBstr name;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  Microsoft::WRL::ComPtr<IAccessibleTableCell> accessible_cell;
  LONG row_index = -1;
  LONG column_index = -1;
  EXPECT_HRESULT_SUCCEEDED(cell1->role(&role));
  EXPECT_EQ(ROLE_SYSTEM_CELL, role);
  EXPECT_HRESULT_SUCCEEDED(cell1->get_accName(childid_self, name.Receive()));
  EXPECT_STREQ(L"AD", static_cast<BSTR>(name));
  EXPECT_HRESULT_SUCCEEDED(cell1.CopyTo(accessible_cell.GetAddressOf()));
  EXPECT_HRESULT_SUCCEEDED(accessible_cell->get_rowIndex(&row_index));
  EXPECT_HRESULT_SUCCEEDED(accessible_cell->get_columnIndex(&column_index));
  EXPECT_EQ(0, row_index);
  EXPECT_EQ(0, column_index);
  name.Reset();
  accessible_cell.Reset();

  // The first cell has a rowspan of 2, try navigating down and expect to get
  // at the end of the table.
  base::win::ScopedVariant variant;
  EXPECT_HRESULT_SUCCEEDED(
      cell1->accNavigate(NAVDIR_DOWN, childid_self, variant.Receive()));
  ASSERT_EQ(VT_EMPTY, variant.type());

  // Try navigating to the cell in the first row, 2nd column.
  Microsoft::WRL::ComPtr<IAccessible2> cell2;
  EXPECT_HRESULT_SUCCEEDED(
      cell1->accNavigate(NAVDIR_RIGHT, childid_self, variant.Receive()));
  ASSERT_NE(nullptr, V_DISPATCH(variant.AsInput()));
  ASSERT_EQ(VT_DISPATCH, variant.type());
  V_DISPATCH(variant.AsInput())->QueryInterface(cell2.GetAddressOf());
  EXPECT_HRESULT_SUCCEEDED(cell2->role(&role));
  EXPECT_EQ(ROLE_SYSTEM_CELL, role);
  EXPECT_HRESULT_SUCCEEDED(cell2->get_accName(childid_self, name.Receive()));
  EXPECT_STREQ(L"BC", static_cast<BSTR>(name));
  EXPECT_HRESULT_SUCCEEDED(cell2.CopyTo(accessible_cell.GetAddressOf()));
  EXPECT_HRESULT_SUCCEEDED(accessible_cell->get_rowIndex(&row_index));
  EXPECT_HRESULT_SUCCEEDED(accessible_cell->get_columnIndex(&column_index));
  EXPECT_EQ(0, row_index);
  EXPECT_EQ(1, column_index);
  variant.Reset();
  name.Reset();
  accessible_cell.Reset();

  // Try navigating to the cell in the second row, 2nd column.
  Microsoft::WRL::ComPtr<IAccessible2> cell3;
  EXPECT_HRESULT_SUCCEEDED(
      cell2->accNavigate(NAVDIR_DOWN, childid_self, variant.Receive()));
  ASSERT_NE(nullptr, V_DISPATCH(variant.AsInput()));
  ASSERT_EQ(VT_DISPATCH, variant.type());
  V_DISPATCH(variant.AsInput())->QueryInterface(cell3.GetAddressOf());
  EXPECT_HRESULT_SUCCEEDED(cell3->role(&role));
  EXPECT_EQ(ROLE_SYSTEM_CELL, role);
  EXPECT_HRESULT_SUCCEEDED(cell3->get_accName(childid_self, name.Receive()));
  EXPECT_STREQ(L"EF", static_cast<BSTR>(name));
  EXPECT_HRESULT_SUCCEEDED(cell3.CopyTo(accessible_cell.GetAddressOf()));
  EXPECT_HRESULT_SUCCEEDED(accessible_cell->get_rowIndex(&row_index));
  EXPECT_HRESULT_SUCCEEDED(accessible_cell->get_columnIndex(&column_index));
  EXPECT_EQ(1, row_index);
  EXPECT_EQ(1, column_index);
  variant.Reset();
  name.Reset();
  accessible_cell.Reset();
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestTreegridIsIATable) {
  ASSERT_TRUE(embedded_test_server()->Start());
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kLoadComplete);
  EXPECT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL(
                                 "/accessibility/aria/aria-treegrid.html")));
  waiter.WaitForNotification();

  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  // There are two treegrids in this test file. Use only the first one.
  ASSERT_EQ(2u, document_children.size());

  Microsoft::WRL::ComPtr<IAccessible2> table;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      &table));
  LONG role = 0;
  ASSERT_HRESULT_SUCCEEDED(table->role(&role));
  ASSERT_EQ(ROLE_SYSTEM_OUTLINE, role);

  // Retrieve the first cell.
  Microsoft::WRL::ComPtr<IAccessibleTable2> table2;
  Microsoft::WRL::ComPtr<IUnknown> cell;
  Microsoft::WRL::ComPtr<IAccessible2> cell1;
  EXPECT_HRESULT_SUCCEEDED(table.CopyTo(IID_PPV_ARGS(&table2)));
  EXPECT_HRESULT_SUCCEEDED(table2->get_cellAt(0, 0, &cell));
  EXPECT_HRESULT_SUCCEEDED(cell.CopyTo(IID_PPV_ARGS(&cell1)));

  base::win::ScopedVariant childid_self(CHILDID_SELF);
  Microsoft::WRL::ComPtr<IAccessibleTableCell> accessible_cell;
  LONG row_index = -1;
  LONG column_index = -1;
  EXPECT_HRESULT_SUCCEEDED(cell1->role(&role));
  EXPECT_EQ(ROLE_SYSTEM_CELL, role);
  EXPECT_HRESULT_SUCCEEDED(cell1.CopyTo(IID_PPV_ARGS(&accessible_cell)));
  EXPECT_HRESULT_SUCCEEDED(accessible_cell->get_rowIndex(&row_index));
  EXPECT_HRESULT_SUCCEEDED(accessible_cell->get_columnIndex(&column_index));
  EXPECT_EQ(0, row_index);
  EXPECT_EQ(0, column_index);
  accessible_cell.Reset();
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestScrollTo) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
      <html>
      <body>
        <div style="height: 5000px;"></div>
        <img src="" alt="Target1">
        <div style="height: 5000px;"></div>
        <img src="" alt="Target2">
        <div style="height: 5000px;"></div>
      </body>
      </html>)HTML");

  // Retrieve the IAccessible interface for the document node.
  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());

  // Get the dimensions of the document.
  LONG doc_x, doc_y, doc_width, doc_height;
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  ASSERT_HRESULT_SUCCEEDED(document->accLocation(&doc_x, &doc_y, &doc_width,
                                                 &doc_height, childid_self));

  // The document should only have two children, both with a role of GRAPHIC.
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(2u, document_children.size());
  Microsoft::WRL::ComPtr<IAccessible2> target;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      target.GetAddressOf()));
  LONG target_role = 0;
  ASSERT_HRESULT_SUCCEEDED(target->role(&target_role));
  ASSERT_EQ(ROLE_SYSTEM_GRAPHIC, target_role);
  Microsoft::WRL::ComPtr<IAccessible2> target2;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[1].AsInput())
          .Get(),
      target2.GetAddressOf()));
  LONG target2_role = 0;
  ASSERT_HRESULT_SUCCEEDED(target2->role(&target2_role));
  ASSERT_EQ(ROLE_SYSTEM_GRAPHIC, target2_role);

  // Call scrollTo on the first target. Ensure it ends up very near the
  // center of the window.
  AccessibilityNotificationWaiter waiter(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kScrollPositionChanged);
  ASSERT_HRESULT_SUCCEEDED(target->scrollTo(IA2_SCROLL_TYPE_ANYWHERE));
  waiter.WaitForNotification();

  // Don't assume anything about the font size or the exact centering
  // behavior, just assert that the object is (roughly) centered by
  // checking that its top coordinate is between 40% and 60% of the
  // document's height.
  LONG x, y, width, height;
  ASSERT_HRESULT_SUCCEEDED(
      target->accLocation(&x, &y, &width, &height, childid_self));
  EXPECT_GT(y + height / 2, doc_y + 0.4 * doc_height);
  EXPECT_LT(y + height / 2, doc_y + 0.6 * doc_height);

  // Now call scrollTo on the second target. Ensure it ends up very near the
  // center of the window.
  AccessibilityNotificationWaiter waiter2(
      shell()->web_contents(), ui::kAXModeComplete,
      ax::mojom::Event::kScrollPositionChanged);
  ASSERT_HRESULT_SUCCEEDED(target2->scrollTo(IA2_SCROLL_TYPE_ANYWHERE));
  waiter2.WaitForNotification();

  // Same as above, make sure it's roughly centered.
  ASSERT_HRESULT_SUCCEEDED(
      target2->accLocation(&x, &y, &width, &height, childid_self));
  EXPECT_GT(y + height / 2, doc_y + 0.4 * doc_height);
  EXPECT_LT(y + height / 2, doc_y + 0.6 * doc_height);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestPageIsAccessibleAfterCancellingReload) {
  LoadInitialAccessibilityTreeFromHtml(
      "data:text/html,"
      "<script>"
      "window.onbeforeunload = function () {"
      "  return '';"
      "};"
      "</script>"
      "<input value='Test'>");

  // When the before unload dialog shows, simulate the user clicking
  // cancel on that dialog.
  SetShouldProceedOnBeforeUnload(shell(), true, false);

  // The beforeunload dialog won't be shown unless the page has at
  // least one user gesture on it.
  auto* main_frame = shell()->web_contents()->GetMainFrame();
  main_frame->ExecuteJavaScriptWithUserGestureForTests(L"");

  // Trigger a reload here, which will get cancelled.
  shell()->web_contents()->GetController().Reload(ReloadType::NORMAL, false);

  // Wait for the dialog to be triggered and then get cancelled.
  WaitForAppModalDialog(shell());

  // Now set up a listener for native Windows accessibility events.
  // The bug here was that when a page is being reloaded or navigated
  // away, we were suppressing accessibility events. This test ensures
  // that if you cancel a reload, events are no longer suppressed.
  WebContentsImpl* web_contents =
      static_cast<WebContentsImpl*>(shell()->web_contents());
  BrowserAccessibilityManager* manager =
      web_contents->GetRootBrowserAccessibilityManager();
  NativeWinEventWaiter win_event_waiter(
      manager, "EVENT_OBJECT_FOCUS on <input> role=ROLE_SYSTEM_TEXT*");

  // Get the root accessible element and its children.
  Microsoft::WRL::ComPtr<IAccessible> document(GetRendererAccessible());
  std::vector<base::win::ScopedVariant> document_children =
      GetAllAccessibleChildren(document.Get());
  ASSERT_EQ(1u, document_children.size());

  // Get the only child of the root.
  Microsoft::WRL::ComPtr<IAccessible2> group;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(document.Get(), document_children[0].AsInput())
          .Get(),
      group.GetAddressOf()));
  LONG group_role = 0;
  ASSERT_HRESULT_SUCCEEDED(group->role(&group_role));
  ASSERT_EQ(IA2_ROLE_SECTION, group_role);
  std::vector<base::win::ScopedVariant> group_children =
      GetAllAccessibleChildren(group.Get());
  ASSERT_EQ(2u, group_children.size());

  // Get the second child of that group, the input element.
  Microsoft::WRL::ComPtr<IAccessible2> input;
  ASSERT_HRESULT_SUCCEEDED(QueryIAccessible2(
      GetAccessibleFromVariant(group.Get(), group_children[1].AsInput()).Get(),
      input.GetAddressOf()));
  LONG input_role = 0;
  ASSERT_HRESULT_SUCCEEDED(input->role(&input_role));
  ASSERT_EQ(ROLE_SYSTEM_TEXT, input_role);

  // Try to focus that input element.
  base::win::ScopedVariant childid_self(CHILDID_SELF);
  HRESULT hr = input->accSelect(SELFLAG_TAKEFOCUS, childid_self);
  ASSERT_EQ(S_OK, hr);

  // Ensure that we get the native focus event on that input element.
  win_event_waiter.Wait();
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest, TestIScrollProvider) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(
      <!DOCTYPE html>
      <html>
        <body>
          <div aria-label='not'>
            not scrollable
          </div>
          <div style='width:100px; overflow:auto' aria-label='x'>
              <div style='width:200px; height:100px'></div>
          </div>
          <div style='height:100px; overflow:auto' aria-label='y'>
              <div style='width:100px; height:200px'></div>
          </div>
        </body>
      </html>
    )HTML");

  struct ScrollTestData {
    std::string node_name;
    bool can_scroll_horizontal;
    bool can_scroll_vertical;
    double size_horizontal;
    double size_vertical;
  };
  double error = 0.01f;
  std::vector<ScrollTestData> all_expected = {{"not", false, false, 0.0, 0.0},
                                              {"x", true, false, 50.0, 0.0},
                                              {"y", false, true, 0.0, 50.0}};
  for (auto& expected : all_expected) {
    BrowserAccessibility* browser_accessibility =
        FindNode(ax::mojom::Role::kGenericContainer, expected.node_name);
    EXPECT_NE(browser_accessibility, nullptr);

    BrowserAccessibilityComWin* browser_accessibility_com_win =
        ToBrowserAccessibilityWin(browser_accessibility)->GetCOM();
    Microsoft::WRL::ComPtr<IScrollProvider> scroll_provider;

    EXPECT_HRESULT_SUCCEEDED(browser_accessibility_com_win->GetPatternProvider(
        UIA_ScrollPatternId, &scroll_provider));

    if (expected.can_scroll_vertical || expected.can_scroll_horizontal) {
      ASSERT_NE(nullptr, scroll_provider);

      BOOL can_scroll_horizontal;
      EXPECT_HRESULT_SUCCEEDED(
          scroll_provider->get_HorizontallyScrollable(&can_scroll_horizontal));
      ASSERT_EQ(expected.can_scroll_horizontal, can_scroll_horizontal);
      if (expected.can_scroll_horizontal) {
        double size_horizontal;
        EXPECT_HRESULT_SUCCEEDED(
            scroll_provider->get_HorizontalViewSize(&size_horizontal));
        EXPECT_NEAR(expected.size_horizontal, size_horizontal, error);

        double x_before;
        EXPECT_HRESULT_SUCCEEDED(
            scroll_provider->get_HorizontalScrollPercent(&x_before));
        EXPECT_NEAR(0, x_before, error);

        AccessibilityNotificationWaiter waiter(
            shell()->web_contents(), ui::kAXModeComplete,
            ax::mojom::Event::kScrollPositionChanged);

        EXPECT_HRESULT_SUCCEEDED(scroll_provider->Scroll(
            ScrollAmount_SmallIncrement, ScrollAmount_NoAmount));
        waiter.WaitForNotification();

        double x_after;
        EXPECT_HRESULT_SUCCEEDED(
            scroll_provider->get_HorizontalScrollPercent(&x_after));
        EXPECT_GT(x_after, x_before);

        AccessibilityNotificationWaiter waiter2(
            shell()->web_contents(), ui::kAXModeComplete,
            ax::mojom::Event::kScrollPositionChanged);

        EXPECT_HRESULT_SUCCEEDED(scroll_provider->SetScrollPercent(0.0, 0.0));
        waiter2.WaitForNotification();

        EXPECT_HRESULT_SUCCEEDED(
            scroll_provider->get_HorizontalScrollPercent(&x_after));
        EXPECT_NEAR(0, x_after, error);
      }

      BOOL can_scroll_vertical;
      EXPECT_HRESULT_SUCCEEDED(
          scroll_provider->get_VerticallyScrollable(&can_scroll_vertical));
      ASSERT_EQ(expected.can_scroll_vertical, can_scroll_vertical);
      if (expected.can_scroll_vertical) {
        double size_vertical;
        EXPECT_HRESULT_SUCCEEDED(
            scroll_provider->get_VerticalViewSize(&size_vertical));
        EXPECT_NEAR(expected.size_vertical, size_vertical, error);

        double y_before;
        EXPECT_HRESULT_SUCCEEDED(
            scroll_provider->get_VerticalScrollPercent(&y_before));
        EXPECT_NEAR(0, y_before, error);

        AccessibilityNotificationWaiter waiter(
            shell()->web_contents(), ui::kAXModeComplete,
            ax::mojom::Event::kScrollPositionChanged);

        EXPECT_HRESULT_SUCCEEDED(scroll_provider->Scroll(
            ScrollAmount_NoAmount, ScrollAmount_SmallIncrement));
        waiter.WaitForNotification();

        double y_after;
        EXPECT_HRESULT_SUCCEEDED(
            scroll_provider->get_VerticalScrollPercent(&y_after));
        EXPECT_GT(y_after, y_before);

        AccessibilityNotificationWaiter waiter2(
            shell()->web_contents(), ui::kAXModeComplete,
            ax::mojom::Event::kScrollPositionChanged);

        EXPECT_HRESULT_SUCCEEDED(scroll_provider->SetScrollPercent(0.0, 0.0));
        waiter2.WaitForNotification();

        EXPECT_HRESULT_SUCCEEDED(
            scroll_provider->get_VerticalScrollPercent(&y_after));
        EXPECT_NEAR(0, y_after, error);
      }
    } else {
      EXPECT_EQ(nullptr, scroll_provider);
    }
  }
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinBrowserTest,
                       TestIsContentElementPropertyId) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
      <html>
        <table>
          <tr>
              <th aria-label="header">
                  header
              </th>
              <td> data </td>
          </tr>
        </table>
      </html>)HTML");

  BrowserAccessibility* target =
      FindNode(ax::mojom::Role::kRowHeader, "header");
  EXPECT_NE(nullptr, target);
  BrowserAccessibilityComWin* accessibility_com_win =
      ToBrowserAccessibilityWin(target)->GetCOM();
  EXPECT_NE(nullptr, accessibility_com_win);

  base::win::ScopedVariant result;
  accessibility_com_win->GetPropertyValue(UIA_IsContentElementPropertyId,
                                          result.Receive());

  BrowserAccessibility* child = target->PlatformDeepestFirstChild();
  EXPECT_NE(nullptr, child);
  accessibility_com_win = ToBrowserAccessibilityWin(child)->GetCOM();
  EXPECT_NE(nullptr, accessibility_com_win);

  result.Release();
  accessibility_com_win->GetPropertyValue(UIA_IsContentElementPropertyId,
                                          result.Receive());
  EXPECT_EQ(VT_BOOL, result.type());
  EXPECT_EQ(VARIANT_FALSE, result.ptr()->boolVal);
}

class AccessibilityWinUIABrowserTest : public AccessibilityWinBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    AccessibilityWinBrowserTest::SetUpCommandLine(command_line);

    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        ::switches::kEnableExperimentalUIAutomation);
  }
};

IN_PROC_BROWSER_TEST_F(AccessibilityWinUIABrowserTest,
                       TestIFrameRootNodeChange) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
      <html>
      </html>)HTML");

  // Request an automation element for the legacy window to ensure that the
  // fragment root is created before the iframe content tree shows up.
  Microsoft::WRL::ComPtr<IUIAutomation> uia;
  ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_IUIAutomation, &uia));

  RenderWidgetHostViewAura* render_widget_host_view_aura =
      static_cast<RenderWidgetHostViewAura*>(
          shell()->web_contents()->GetRenderWidgetHostView());
  ASSERT_NE(nullptr, render_widget_host_view_aura);
  HWND hwnd = render_widget_host_view_aura->AccessibilityGetAcceleratedWidget();
  ASSERT_NE(gfx::kNullAcceleratedWidget, hwnd);
  Microsoft::WRL::ComPtr<IUIAutomationElement> root_element;
  ASSERT_HRESULT_SUCCEEDED(uia->ElementFromHandle(hwnd, &root_element));
  ASSERT_NE(nullptr, root_element.Get());

  // Insert a new iframe and wait for tree update.
  AccessibilityNotificationWaiter waiter(shell()->web_contents(),
                                         ui::kAXModeComplete,
                                         ax::mojom::Event::kLayoutComplete);
  ExecuteScript(
      L"let new_frame = document.createElement('iframe');"
      L"new_frame.setAttribute('src', 'about:blank');"
      L"document.body.appendChild(new_frame);");
  waiter.WaitForNotification();

  // Content root node's parent's child should still be the content root node.
  Microsoft::WRL::ComPtr<IRawElementProviderFragment> content_root;
  ASSERT_HRESULT_SUCCEEDED(
      GetManager()->GetRoot()->GetNativeViewAccessible()->QueryInterface(
          IID_PPV_ARGS(&content_root)));

  Microsoft::WRL::ComPtr<IRawElementProviderFragment> parent;
  ASSERT_HRESULT_SUCCEEDED(
      content_root->Navigate(NavigateDirection_Parent, &parent));
  ASSERT_NE(nullptr, parent.Get());

  Microsoft::WRL::ComPtr<IRawElementProviderFragment> first_child;
  ASSERT_HRESULT_SUCCEEDED(
      parent->Navigate(NavigateDirection_FirstChild, &first_child));
  EXPECT_EQ(content_root.Get(), first_child.Get());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinUIABrowserTest, TestGetFragmentRoot) {
  // Verify that we can obtain a fragment root from a fragment without having
  // sent WM_GETOBJECT to the host window.
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
      <html>
      </html>)HTML");

  Microsoft::WRL::ComPtr<IRawElementProviderFragment> content_root;
  ASSERT_HRESULT_SUCCEEDED(
      GetManager()->GetRoot()->GetNativeViewAccessible()->QueryInterface(
          IID_PPV_ARGS(&content_root)));

  Microsoft::WRL::ComPtr<IRawElementProviderFragmentRoot> fragment_root;
  ASSERT_HRESULT_SUCCEEDED(content_root->get_FragmentRoot(&fragment_root));
  ASSERT_NE(nullptr, fragment_root.Get());
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinUIABrowserTest,
                       RootElementPropertyValues) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
      <html>
        <title>Page title</title>
      </html>)HTML");

  // Request an automation element for the root element.
  Microsoft::WRL::ComPtr<IUIAutomation> uia;
  ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_IUIAutomation, &uia));

  HWND hwnd = shell()->window()->GetHost()->GetAcceleratedWidget();
  ASSERT_NE(gfx::kNullAcceleratedWidget, hwnd);
  Microsoft::WRL::ComPtr<IUIAutomationElement> element;
  ASSERT_HRESULT_SUCCEEDED(uia->ElementFromHandle(hwnd, &element));
  ASSERT_NE(nullptr, element.Get());

  // The control type should be Window and the name should be the same as the
  // window title. These values come from UIA's built-in provider; this test
  // validates that we aren't overriding them.
  CONTROLTYPEID control_type;
  ASSERT_HRESULT_SUCCEEDED(element->get_CurrentControlType(&control_type));
  EXPECT_EQ(UIA_WindowControlTypeId, control_type);

  wchar_t window_text[100] = {0};
  ::GetWindowTextW(hwnd, window_text, _countof(window_text));
  base::string16 window_text_str16(window_text);
  base::win::ScopedBstr name;
  ASSERT_HRESULT_SUCCEEDED(element->get_CurrentName(name.Receive()));
  base::string16 name_str16(name, name.Length());
  EXPECT_EQ(window_text_str16, name_str16);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinUIABrowserTest,
                       LegacyWindowIsNotControlElement) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
        <html>
        </html>)HTML");

  // Request an automation element for the legacy window.
  Microsoft::WRL::ComPtr<IUIAutomation> uia;
  ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_IUIAutomation, &uia));

  RenderWidgetHostViewAura* render_widget_host_view_aura =
      static_cast<RenderWidgetHostViewAura*>(
          shell()->web_contents()->GetRenderWidgetHostView());
  ASSERT_NE(nullptr, render_widget_host_view_aura);
  HWND hwnd = render_widget_host_view_aura->AccessibilityGetAcceleratedWidget();
  ASSERT_NE(gfx::kNullAcceleratedWidget, hwnd);
  Microsoft::WRL::ComPtr<IUIAutomationElement> element;
  ASSERT_HRESULT_SUCCEEDED(uia->ElementFromHandle(hwnd, &element));
  ASSERT_NE(nullptr, element.Get());

  // The legacy window should not be in the control or content trees.
  BOOL result;
  ASSERT_HRESULT_SUCCEEDED(element->get_CurrentIsControlElement(&result));
  EXPECT_EQ(FALSE, result);
  ASSERT_HRESULT_SUCCEEDED(element->get_CurrentIsContentElement(&result));
  EXPECT_EQ(FALSE, result);
}

IN_PROC_BROWSER_TEST_F(AccessibilityWinUIABrowserTest,
                       UIAParentNavigationDuringWebContentsClose) {
  LoadInitialAccessibilityTreeFromHtml(
      R"HTML(<!DOCTYPE html>
        <html>
        </html>)HTML");

  // Request an automation element for UIA tree traversal.
  Microsoft::WRL::ComPtr<IUIAutomation> uia;
  ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_IUIAutomation, &uia));

  RenderWidgetHostViewAura* view = static_cast<RenderWidgetHostViewAura*>(
      shell()->web_contents()->GetRenderWidgetHostView());
  ASSERT_NE(nullptr, view);

  // Start by getting the root element for the HWND hosting the web content.
  HWND hwnd = view->host()
                  ->GetRootBrowserAccessibilityManager()
                  ->GetRoot()
                  ->GetTargetForNativeAccessibilityEvent();
  ASSERT_NE(gfx::kNullAcceleratedWidget, hwnd);
  Microsoft::WRL::ComPtr<IUIAutomationElement> root;
  uia->ElementFromHandle(hwnd, &root);
  ASSERT_NE(nullptr, root.Get());

  Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> tree_walker;
  uia->get_RawViewWalker(&tree_walker);
  ASSERT_NE(nullptr, tree_walker.Get());

  // Navigate to the root's first child before closing the WebContents.
  Microsoft::WRL::ComPtr<IUIAutomationElement> first_child;
  tree_walker->GetFirstChildElement(root.Get(), &first_child);
  ASSERT_NE(nullptr, first_child.Get());

  // The bug only reproduces during the WebContentsDestroyed event, so create
  // an observer that will do UIA parent navigation (on the first child that
  // was just obtained) while the WebContents is being destroyed.
  content::WebContentsUIAParentNavigationInDestroyedWatcher destroyed_watcher(
      shell()->web_contents(), first_child.Get(), tree_walker.Get());
  shell()->CloseContents(shell()->web_contents());
  destroyed_watcher.Wait();
}

}  // namespace content
