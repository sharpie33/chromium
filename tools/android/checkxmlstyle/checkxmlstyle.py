# Copyright 2018 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Script that is used by PRESUBMIT.py to check Android XML files.

This file checks for the following:
  - Colors are defined as RRGGBB or AARRGGBB
  - No (A)RGB values are referenced outside color_palette.xml
  - No duplicate (A)RGB values are referenced in color_palette.xml
  - Colors in semantic_colors are only referecing colors in color_palette.xml
  - XML namspace "app" is used for "http://schemas.android.com/apk/res-auto"
  - Android text attributes are only defined in text appearance styles
  - Warning on adding new text appearance styles
"""

from collections import defaultdict
import os
import re
import xml.etree.ElementTree as ET

import helpers

def CheckStyleOnUpload(input_api, output_api):
  """Returns result for all the presubmit upload checks for XML files."""
  result = _CommonChecks(input_api, output_api)
  result.extend(_CheckNewTextAppearance(input_api, output_api))
  return result


def CheckStyleOnCommit(input_api, output_api):
  """Returns result for all the presubmit commit checks for XML files."""
  return _CommonChecks(input_api, output_api)


def IncludedFiles(input_api):
  # Filter out XML files outside included paths and files that were deleted.
  files = lambda f: input_api.FilterSourceFile(
      f, white_list=helpers.INCLUDED_PATHS)
  return input_api.AffectedFiles(include_deletes=False, file_filter=files)


def _CommonChecks(input_api, output_api):
  """Checks common to both upload and commit."""
  result = []
  result.extend(_CheckColorFormat(input_api, output_api))
  result.extend(_CheckColorReferences(input_api, output_api))
  result.extend(_CheckDuplicateColors(input_api, output_api))
  result.extend(_CheckSemanticColorsReferences(input_api, output_api))
  result.extend(_CheckXmlNamespacePrefixes(input_api, output_api))
  result.extend(_CheckTextAppearance(input_api, output_api))
  result.extend(_CheckButtonCompatWidgetUsage(input_api, output_api))
  # Add more checks here
  return result

### color resources below ###
def _CheckColorFormat(input_api, output_api):
  """Checks color (A)RGB values are of format either RRGGBB or AARRGGBB."""
  errors = []
  for f in IncludedFiles(input_api):
    # Ignore vector drawable xmls
    contents = input_api.ReadFile(f)
    if '<vector' in contents:
      continue
    for line_number, line in f.ChangedContents():
      color = helpers.COLOR_PATTERN.search(line)
      if color and not helpers.VALID_COLOR_PATTERN.match(color.group(2)):
        errors.append(
            '  %s:%d\n    \t%s' % (f.LocalPath(), line_number, line.strip()))
  if errors:
    return [
        output_api.PresubmitError(
            '''
  Android Color Reference Check failed:
    Your new code added (A)RGB values for colors that are not well
    formatted, listed below.

    This is banned, please define colors in format of #RRGGBB for opaque
    colors or #AARRGGBB for translucent colors. Note that they should be
    defined in chrome/android/java/res/values/colors.xml.

    See https://crbug.com/775198 for more information.
  ''', errors)
    ]
  return []


def _CheckColorReferences(input_api, output_api):
  """Checks no (A)RGB values are defined outside color_palette.xml."""
  errors = []
  warnings = []
  for f in IncludedFiles(input_api):
    if f.LocalPath() == helpers.COLOR_PALETTE_RELATIVE_PATH:
      continue
    # Ignore new references in vector/shape drawable xmls
    contents = input_api.ReadFile(f)
    is_vector_drawable = '<vector' in contents or '<shape' in contents
    for line_number, line in f.ChangedContents():
      if helpers.COLOR_PATTERN.search(line):
        issue = '  %s:%d\n    \t%s' % (f.LocalPath(), line_number, line.strip())
        if is_vector_drawable:
          warnings.append(issue)
        else:
          errors.append(issue)
  result = []
  if errors:
    result += [
        output_api.PresubmitError(
            '''
  Android Color Reference Check failed:
    Your new code added new color references that are not color resources from
    ui/android/java/res/values/color_palette.xml, listed below.

    This is banned, please use the existing color resources or create a new
    color resource in colors.xml, and reference the color by @color/....

    See https://crbug.com/775198 for more information.
  ''', errors)
    ]
  if warnings:
    result += [
        output_api.PresubmitPromptWarning(
            '''
  Android Color Reference Check warning:
    Your new code added new color references that are not color resources from
    ui/android/java/res/values/color_palette.xml, listed below.

    This is typically not needed even in vector/shape drawables. Please consider
    using an existing color resources if possible.

    Only bypass this check if you are confident that you should be using a HEX
    reference, e.g. you are adding an illustration or a shadow using XML rather
    than a PNG/9-patch.

    Please contact src/chrome/android/java/res/OWNERS for questions.
  ''', warnings)
    ]
  return result


def _CheckDuplicateColors(input_api, output_api):
  """Checks colors defined by (A)RGB values in color_palette.xml are unique."""
  errors = []
  for f in IncludedFiles(input_api):
    if f.LocalPath() != helpers.COLOR_PALETTE_RELATIVE_PATH:
      continue
    colors = defaultdict(int)
    contents = input_api.ReadFile(f)
    # Get count for each color defined.
    for line in contents.splitlines(False):
      color = helpers.COLOR_PATTERN.search(line)
      if color:
        colors[color.group(2)] += 1

    # Check duplicates in changed contents.
    for line_number, line in f.ChangedContents():
      color = helpers.COLOR_PATTERN.search(line)
      if color and colors[color.group(2)] > 1:
        errors.append(
            '  %s:%d\n    \t%s' % (f.LocalPath(), line_number, line.strip()))
  if errors:
    return [
        output_api.PresubmitError(
            '''
  Android Duplicate Color Declaration Check failed:
    Your new code added new colors by (A)RGB values that are already defined in
    ui/android/java/res/values/color_palette.xml, listed below.

    This is banned, please reference the existing color resource from colors.xml
    using @color/... and if needed, give the existing color resource a more
    general name (e.g. modern_grey_100).

    See https://crbug.com/775198 for more information.
  ''', errors)
    ]
  return []


def _CheckSemanticColorsReferences(input_api, output_api):
  """
  Checks colors defined in semantic_colors.xml only referencing
  resources in color_palette.xml
  """
  errors = []
  color_palette = None

  for f in IncludedFiles(input_api):
    if not f.LocalPath().endswith('/semantic_colors.xml'):
      continue

    if color_palette is None:
      color_palette = _colorXml2Dict(
        input_api.ReadFile(helpers.COLOR_PALETTE_PATH))
    for line_number, line in f.ChangedContents():
      r = helpers.COLOR_REFERENCE_PATTERN.search(line)
      if not r:
        continue
      color = r.group()
      if _removePrefix(color) not in color_palette:
        errors.append(
            '  %s:%d\n    \t%s' % (f.LocalPath(), line_number, line.strip()))

  if errors:
    return [
        output_api.PresubmitError(
            '''
  Android Semantic Color Reference Check failed:
    Your new color values added in semantic_colors are not defined in
    ui/android/java/res/values/color_palette.xml, listed below.

    This is banned. Colors in semantic colors can only reference
    the existing color resource from color_palette.xml.

    See https://crbug.com/775198 for more information.
  ''', errors)
    ]
  return []


def _CheckXmlNamespacePrefixes(input_api, output_api):
  """Checks consistency of prefixes used for XML namespace names."""
  errors = []
  for f in IncludedFiles(input_api):
    for line_number, line in f.ChangedContents():
      xml_app_namespace = helpers.XML_APP_NAMESPACE_PATTERN.search(line)
      if xml_app_namespace and not xml_app_namespace.group(1) == 'app':
        errors.append(
            '  %s:%d\n    \t%s' % (f.LocalPath(), line_number, line.strip()))
  if errors:
    return [
        output_api.PresubmitError(
            '''
  XML Namespace Prefixes Check failed:
    Your new code added new xml namespace declaration that is not consistent
    with other XML files. Namespace "http://schemas.android.com/apk/res-auto"
    should use 'app' prefix:

    xmlns:app="http://schemas.android.com/apk/res-auto"

    See https://crbug.com/850616 for more information.
  ''', errors)
    ]
  return []


### text appearance below ###
def _CheckTextAppearance(input_api, output_api):
  """Checks text attributes are only used for text appearance styles in XMLs."""
  text_attributes = [
      'android:textColor', 'android:textSize', 'android:textStyle',
      'android:fontFamily', 'android:textAllCaps']
  namespace = {'android': 'http://schemas.android.com/apk/res/android'}
  errors = []
  for f in IncludedFiles(input_api):
    root = ET.fromstring(input_api.ReadFile(f))
    # Check if there are text attributes defined outside text appearances.
    for attribute in text_attributes:
      # Get style name that contains text attributes but is not text appearance.
      invalid_styles = []
      for style in root.findall('style') + root.findall('.//style'):
        name = style.get('name')
        is_text_appearance = helpers.TEXT_APPEARANCE_STYLE_PATTERN.search(name)
        item = style.find(".//item[@name='"+attribute+"']")
        if is_text_appearance is None and item is not None:
          invalid_styles.append(name)
      # Append error messages.
      contents = input_api.ReadFile(f)
      style_count = 0
      widget_count = len(root.findall('[@'+attribute+']', namespace)) + len(
          root.findall('.//*[@'+attribute+']', namespace))
      for line_number, line in enumerate(contents.splitlines(False)):
        # Error for text attributes in non-text-appearance style.
        if (style_count < len(invalid_styles) and
            invalid_styles[style_count] in line):
          errors.append('  %s:%d contains attribute %s\n    \t%s' % (
              f.LocalPath(), line_number+1, attribute, line.strip()))
          style_count += 1
        # Error for text attributes in layout.
        if widget_count > 0 and attribute in line:
          errors.append('  %s:%d contains attribute %s\n    \t%s' % (
              f.LocalPath(), line_number+1, attribute, line.strip()))
          widget_count -= 1
  # TODO(huayinz): Change the path on the error message to the corresponding
  # styles.xml when this check applies to all resource directories.
  if errors:
    return [
        output_api.PresubmitError(
            '''
  Android Text Appearance Check failed:
    Your modified files contain Android text attributes defined outside
    text appearance styles, listed below.

    It is recommended to use the pre-defined text appearance styles in
      src/ui/android/java/res/values-v17/styles.xml

    And to use
      android:textAppearance="@style/SomeTextAppearance"
    in the XML layout whenever possible.

    If your text appearance absolutely has to deviate from the existing
    pre-defined text appearance style, you will need UX approval for adding a
    new text appearance style.

    If your approved text appearance style is a common text appreance style,
    please define it in src/ui/android/java/res/values-v17/styles.xml.

    Otherwise, if your approved text appearance is feature-specific, in
    chrome/android/java/res/values*/styles.xml, please define
      <style name="TextAppearance.YourTextAppearanceName>
        <item name="android:textColor">...</item>
        <item name="android:textSize">...</item>
        ...
      </style>

    Please contact hannahs@chromium.org for UX approval, and
    src/chrome/android/java/res/OWNERS for questions.
    See https://crbug.com/775198 for more information.
  ''', errors)
    ]
  return []


def _CheckNewTextAppearance(input_api, output_api):
  """Checks whether a new text appearance style is defined."""
  errors = []
  for f in IncludedFiles(input_api):
    for line_number, line in f.ChangedContents():
      if '<style name="TextAppearance.' in line:
        errors.append(
            '  %s:%d\n    \t%s' % (f.LocalPath(), line_number, line.strip()))
  if errors:
    return [
        output_api.PresubmitPromptWarning(
            '''
  New Text Appearance in styles.xml Check failed:
    Your new code added, edited or removed a text appearance style.
    If you are removing or editing an existing text appearance style, or your
    new text appearance style is approved by UX, please bypass this check.

    Otherwise, please contact hannahs@chromium.org for UX approval, and
    src/chrome/android/java/res/OWNERS for questions.
    See https://crbug.com/775198 for more information.
  ''', errors)
    ]
  return []


### unfavored android widgets below ###
def _CheckButtonCompatWidgetUsage(input_api, output_api):
  """Encourage using ButtonCompat rather than Button, AppButtonCompat"""
  warnings = []

  for f in IncludedFiles(input_api):
    # layout resource files
    for line_number, line in f.ChangedContents():
      if (re.search(r'<Button$', line) or
          re.search(r'<android.support.v7.widget.AppCompatButton$', line)):
        warnings.append(
            '  %s:%d\n    \t%s' % (f.LocalPath(), line_number, line.strip()))

  if warnings:
    return [
        output_api.PresubmitPromptWarning(
            '''
  Android Widget Check warning:
    Your new code is using Button or AppCompatButton, listed below.

    Use org.chromium.ui.widget.ButtonCompat instead of Button and
    AppCompatButton if possible; ButtonCompat is a Material-styled button with a
    customizable background color. On L devices, this is a true Material button.
    On earlier devices, the button is similar but lacks ripples and a shadow.

    See https://crbug.com/775198 and https://crbug.com/908651 for
    more information.
  ''', warnings)
    ]

  return []

### helpers ###
def _colorXml2Dict(content):
  dct = dict()
  tree = ET.fromstring(content)
  for child in tree:
    dct[child.attrib['name']] = child.text
  return dct


def _removePrefix(color, prefix='@color/'):
  if color.startswith(prefix):
    return color[len(prefix):]
  return color
