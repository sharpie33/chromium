# Copyright 2019 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from __future__ import print_function

import json
import os
import sys
import shutil
import subprocess
import tempfile

CHROMIUM_PATH = os.path.join(os.path.dirname(__file__), '..', '..', '..', '..')
TOOLS_PERF_PATH = os.path.join(CHROMIUM_PATH, 'tools', 'perf')
sys.path.insert(1, TOOLS_PERF_PATH)

from core.external_modules import pandas

RUNS_USED_FOR_LIMIT_UPDATE = 30
CHANGE_PERCENTAGE_LIMIT = 0.01

SWARMING_PATH = os.path.join(
  CHROMIUM_PATH, 'tools', 'swarming_client', 'swarming.py')
UPPER_LIMITS_DATA_DIR = os.path.join(
  CHROMIUM_PATH, 'testing', 'scripts', 'representative_perf_test_data')


def FetchItemIds(tags, limit):
  """Fetches the item id of tasks described by the tags.

  Args:
    tags: The tags which describe the task such as OS and buildername.
    limit: The number of runs to look at.

  Returns:
    A list containing the item Id of the tasks.
  """
  swarming_attributes = (
    'tasks/list?tags=name:rendering_representative_perf_tests&tags=os:{os}'
    '&tags=buildername:{buildername}&tags=master:chromium.gpu.fyi&state='
    'COMPLETED&fields=cursor,items(task_id)').format(**tags)

  query = [
    SWARMING_PATH, 'query', '-S', 'chromium-swarm.appspot.com', '--limit',
    str(limit), swarming_attributes]
  output = json.loads(subprocess.check_output(query))
  return output.get('items')


def FetchItemData(task_id, benchmark, index, temp_dir):
  """Fetches the performance values (AVG & CI ranges) of tasks.

  Args:
    task_id: The list of item Ids to fetch dat for.
    benchmark: The benchmark these task are on (desktop/mobile).
    index: The index field of the data_frame
    temp_dir: The temp directory to store task data in.

  Returns:
    A data_frame containing the averages and confidence interval ranges.
  """
  output_directory = os.path.abspath(
    os.path.join(temp_dir, task_id))
  query = [
    SWARMING_PATH, 'collect', '-S', 'chromium-swarm.appspot.com',
    '--task-output-dir', output_directory, task_id]
  try:
    subprocess.check_output(query)
  except Exception as e:
    print(e)

  result_file_path = os.path.join(
    output_directory, '0', 'rendering.' + benchmark, 'perf_results.csv')

  try:
    df = pandas.read_csv(result_file_path)
    df = df.loc[df['name'] == 'frame_times']
    df = df[['stories', 'avg', 'ci_095']]
    df['index'] = index
    return df
  except:
    print("CSV results were not produced!")


def GetPercentileValues(benchmark, tags, limit, percentile):
  """Get the percentile value of recent runs described by given tags.

  Given the tags, benchmark this function fetches the data of last {limit}
  runs, and find the percentile value for each story.

  Args:
    benchmark: The benchmark these task are on (desktop/mobile).
    tags: The tags which describe the tasks such as OS and buildername.
    limit: The number of runs to look at.
    percentile: the percentile to return.

  Returns:
    A dictionary with averages and confidence interval ranges calculated
    from the percentile of recent runs.
  """
  items = []
  for tag_set in tags:
    items.extend(FetchItemIds(tag_set, limit))

  dfs = []
  try:
    temp_dir = tempfile.mkdtemp('perf_csvs')
    for idx, item in enumerate(items):
      dfs.append(FetchItemData(item['task_id'], benchmark, idx, temp_dir))
      idx += 1
  finally:
    shutil.rmtree(temp_dir)
  data_frame = pandas.concat(dfs, ignore_index=True)

  if not data_frame.empty:
    avg_df = data_frame.pivot(index='stories', columns='index', values='avg')
    upper_limit = avg_df.quantile(percentile, axis = 1)
    ci_df = data_frame.pivot(index='stories', columns='index', values='ci_095')
    upper_limit_ci = ci_df.quantile(percentile, axis = 1)
    results = {}
    for index in avg_df.index:
      results[index] = {
        'avg': round(upper_limit[index], 3),
        'ci_095': round(upper_limit_ci[index], 3)
      }
    return results


def MeasureNewUpperLimit(old_value, new_value, att_name, max_change):
  # There has been an improvement.
  if new_value < old_value:
    # Decrease the limit gradually in case of improvements.
    new_value = (old_value + new_value) / 2.0

  change_pct = 0.0
  if old_value > 0:
    change_pct = (new_value - old_value) / old_value

  print(
    '  {}:\t\t {} -> {} \t({:.2f}%)'.format(
      att_name, old_value, new_value, change_pct * 100))
  if new_value < 0.01:
    print('WARNING: New selected value is close to 0.')
  return (
      round(new_value, 3),
      max(max_change, abs(change_pct))
  )


def RecalculateUpperLimits(data_point_count):
  """Recalculates the upper limits using the data of recent runs.

  This method replaces the existing JSON file which contains the upper limits
  used by representative perf tests if the changes of upper limits are
  significant.

  Args:
    data_point_count: The number of runs to use for recalculation.
  """
  with open(os.path.join(UPPER_LIMITS_DATA_DIR,
            'platform_specific_tags.json')) as tags_data:
    platform_specific_tags = json.load(tags_data)

  with open(
    os.path.join(
      UPPER_LIMITS_DATA_DIR,
      'representatives_frame_times_upper_limit.json')) as current_data:
    current_upper_limits = json.load(current_data)

  max_change = 0.0
  results = {}
  for platform in platform_specific_tags:
    platform_data = platform_specific_tags[platform]
    print('\n- Processing data ({})'.format(platform))
    results[platform] = GetPercentileValues(
      platform_data['benchmark'], platform_data['tags'],
      data_point_count, 0.95)

    # Loop over results and adjust base on current values.
    for story in results[platform]:
      if story in current_upper_limits[platform]:
        print(story, ':')
        new_avg, max_change = MeasureNewUpperLimit(
          current_upper_limits[platform][story]['avg'],
          results[platform][story]['avg'], 'AVG', max_change)
        results[platform][story]['avg'] = new_avg

        new_ci, max_change = MeasureNewUpperLimit(
          current_upper_limits[platform][story]['ci_095'],
          results[platform][story]['ci_095'], 'CI', max_change)
        results[platform][story]['ci_095'] = new_ci

  if max_change > CHANGE_PERCENTAGE_LIMIT:
    with open(
      os.path.join(
        UPPER_LIMITS_DATA_DIR,
        'representatives_frame_times_upper_limit.json'
      ), 'w') as outfile:
      json.dump(results, outfile, separators=(',', ': '), indent=2)
    print(
      'Upper limits were updated on '
      'representatives_frame_times_upper_limit.json')
  else:
    print('Changes are small, no need for new limits')


if __name__ == '__main__':
  sys.exit(RecalculateUpperLimits(RUNS_USED_FOR_LIMIT_UPDATE))