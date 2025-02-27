#! /usr/bin/env python
# Copyright (c) 2020 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Linker wrapper that performs distributed ThinLTO on Goma.
#
# Usage: Pass the original link command as parameters to this script.
# E.g. original: lld-link -out:foo foo.obj
# Becomes: goma_link.py lld-link -out:foo foo.obj

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import argparse
import errno
import io
import os
import re
import shlex
import subprocess
import sys
from collections import namedtuple
from pipes import quote as shquote
from tempfile import NamedTemporaryFile

# Python 2 has int and long, and we want to use long.  Python 3 only has int,
# which is like long in Python 2.  So we check if long is defined, and, if not,
# define it to be the same as int.
try:
  long
except NameError:
  long = int

# Type returned by analyze_args.
AnalyzeArgsResult = namedtuple('AnalyzeArgsResult', [
    'output', 'linker', 'compiler', 'splitfile', 'index_params', 'codegen',
    'codegen_params', 'final_params'
])


def autoninja():
  """
  Returns the name of the autoninja executable to invoke.
  """
  name = os.path.normpath(
      os.path.join(
          os.path.dirname(__file__), '..', '..', '..', 'third_party',
          'depot_tools', 'autoninja'))
  if os.name == 'nt':
    return name + '.bat'
  else:
    return name


def create_file(path):
  """
  Creates an empty file at path, creating parent directories if needed.
  """
  ensure_dir(os.path.dirname(path))
  open(path, 'wb').close()


def ensure_dir(path):
  """
  Creates path as a directory if it does not already exist.
  """
  if not path:
    return
  try:
    os.makedirs(path)
  except OSError as e:
    if e.errno != errno.EEXIST:
      raise


def exe_suffix():
  if os.name == 'nt':
    return '.exe'
  else:
    return ''


def is_bitcode_file(path):
  """
  Returns True if path contains a LLVM bitcode file, False if not.
  """
  with open(path, 'rb') as f:
    return f.read(4) == b'BC\xc0\xde'


def is_thin_archive(path):
  """
  Returns True if path refers to a thin archive (ar file), False if not.
  """
  with open(path, 'rb') as f:
    return f.read(8) == b'!<thin>\n'


def names_in_archive(path):
  """
  Yields the member names in the archive file at path.
  """
  # Note: This could alternatively be implemented by invoking some
  # external utility, e.g. llvm-ar, which would avoid having logic here
  # to parse thin archives. The current approach was chosen because it
  # avoids spawning additional processes and having an external dependency
  # (in particular, this works even if llvm-ar is not in $PATH).
  with open(path, 'rb') as f:
    long_names = None
    f.seek(8, io.SEEK_CUR)
    while True:
      file_id = f.read(16)
      if len(file_id) == 0:
        break
      f.seek(32, io.SEEK_CUR)
      m = re.match(b'/([0-9]+)', file_id)
      if long_names and m:
        name_pos = long(m.group(1))
        name_end = long_names.find(b'/\n', name_pos)
        name = long_names[name_pos:name_end]
      else:
        name = file_id
      try:
        size = long(f.read(10))
      except:
        sys.stderr.write('While parsing %r, pos %r\n' % (path, f.tell()))
        raise
      # Two entries are special: '/' and '//'. The former is
      # the symbol table, which we skip. The latter is the long
      # file name table, which we read.
      # Anything else is a filename entry which we yield.
      # Every file record ends with two terminating characters
      # which we skip.
      seek_distance = 2
      if file_id == b'/               ':
        # Skip symbol table.
        seek_distance += size + (size & 1)
      elif file_id == b'//              ':
        # Read long name table.
        f.seek(2, io.SEEK_CUR)
        long_names = f.read(size)
        seek_distance = size & 1
      else:
        # Using UTF-8 here gives us a fighting chance if someone decides to use
        # non-US-ASCII characters in a file name, and backslashreplace gives us
        # a human-readable representation of any undecodable bytes we might
        # encounter.
        yield name.decode('UTF-8', 'backslashreplace')
      f.seek(seek_distance, io.SEEK_CUR)


def ninjaenc(s):
  """
  Encodes string s for use in ninja files.
  """
  return s.replace('$', '$$')


def ninjajoin(l):
  """
  Encodes list of strings l to a string encoded for use in a ninja file.
  """
  return ' '.join(map(ninjaenc, l))


def parse_args(args):
  """
  Parses the command line and returns a structure with the results.
  """
  # The basic invocation is to pass in the command line that would be used
  # for a local ThinLTO link. Optionally, this may be preceded by options
  # that set some values for this script. If these optional options are
  # present, they must be followed by '--'.
  ap = argparse.ArgumentParser()
  ap.add_argument('--gomacc', help='path to gomacc.')
  ap.add_argument('--jobs', '-j', help='maximum number of concurrent jobs.')
  try:
    splitpos = args.index('--')
  except:
    splitpos = None
  if splitpos:
    parsed = ap.parse_args(args[1:splitpos])
    rest = args[(splitpos + 1):]
  else:
    parsed = ap.parse_args([])
    rest = args[1:]
  parsed.linker = rest[0]
  parsed.linker_args = rest[1:]
  return parsed


def report_run(cmd, *args, **kwargs):
  """
  Runs a command using subprocess.check_call, first writing the command line
  to standard error.
  """
  sys.stderr.write('%s: %s\n' % (sys.argv[0], ' '.join(map(shquote, cmd))))
  sys.stderr.flush()
  return subprocess.check_call(cmd, *args, **kwargs)


class GomaLinkBase(object):
  """
  Base class used by GomaLinkUnix and GomaLinkWindows.
  """
  # Defaults.
  gomacc = 'gomacc'
  jobs = None

  # These constants should work across platforms.
  LIB_RE = re.compile('.*\\.(?:a|lib)', re.IGNORECASE)
  LTO_RE = re.compile('|'.join((
      '-fsanitize=cfi.*',
      '-flto.*',
      '-fthin.*',
      '-Wl,-plugin-opt=.*',
      '-Wl,--lto.*',
      '-Wl,--thin.*',
  )))
  OBJ_RE = re.compile('(.*)\\.(o(?:bj)?)', re.IGNORECASE)

  def output_path(self, args):
    """
    Analyzes command line arguments in args and returns the output
    path if one is specified by args. If no output path is specified
    by args, returns None.
    """
    i = 2
    while i < len(args):
      output, next_i = self.process_output_param(args, i)
      if output is not None:
        return output
      i = next_i
    return None

  def write_rsp(self, path, params):
    """
    Writes params to a newly created response file at path.
    """
    ensure_dir(os.path.basename(path))
    with open(path, 'wb') as f:
      f.write('\n'.join(map(self.rspenc, params)).encode('UTF-8'))

  def rspenc(self, param):
    """
    Encodes param for use in an rsp file.
    """
    return param.replace('\\%', '%')

  def expand_rsp(self, rspname):
    """
    Returns the parameters found in the response file at rspname.
    """
    with open(rspname) as f:
      return shlex.split(f.read())

  def expand_args_rsps(self, args):
    """
    Yields args, expanding @rsp file references into the commands mentioned
    in the rsp file.
    """
    result = []
    for arg in args:
      if len(arg) > 0 and arg[0] == '@':
        for x in self.expand_rsp(arg[1:]):
          yield x
      else:
        yield arg

  def expand_thin_archives(self, args):
    """
    Yields the parameters in args, with thin archives replaced by a sequence
    of '-start-lib', the member names, and '-end-lib'. This is used to get a
    command line where members of thin archives are mentioned explicitly.
    """
    for arg in args:
      prefix = os.path.dirname(arg)
      if prefix:
        prefix += '/'
      if (self.LIB_RE.match(arg) and os.path.exists(arg)
          and is_thin_archive(arg)):
        yield (self.WL + '-start-lib')
        for name in names_in_archive(arg):
          yield (prefix + name)
        yield (self.WL + '-end-lib')
      else:
        yield (arg)

  def analyze_args(self, args, gen_dir, common_dir, use_common_objects):
    """
    Analyzes the command line arguments in args.
    If no ThinLTO code generation is necessary, returns None.
    Else, returns an AnalyzeArgsResult value.

    Args:
      args: the command line as returned by parse_args().
      gen_dir: directory in which to generate files specific to this target.
      common_dir: directory for file shared among targets.
      use_common_objects: if True, native object files are shared with
        other targets.
    """
    # If we're invoking the NaCl toolchain, don't do distributed code
    # generation.
    if os.path.basename(args.linker).startswith('pnacl-'):
      return None

    if 'clang' in os.path.basename(args.linker):
      compiler = args.linker
    else:
      compiler_dir = os.path.dirname(args.linker)
      if compiler_dir:
        compiler_dir += '/'
      else:
        compiler_dir = ''
      compiler = compiler_dir + 'clang-cl' + exe_suffix()

    if use_common_objects:
      obj_dir = common_dir
    else:
      obj_dir = gen_dir

    common_index = common_dir + '/empty.thinlto.bc'
    index_params = []
    codegen = []
    codegen_params = [
        '-Wno-unused-command-line-argument',
        '-Wno-override-module',
    ]
    final_params = []
    in_mllvm = [False]
    optlevel = [2]

    MLLVM_RE = re.compile('(?:-Wl,)?([-/]mllvm)[:=]?(.*)', re.IGNORECASE)

    def transform_codegen_param(param):
      """
      If param is a parameter relevant to code generation, returns the
      parameter in a form that is suitable to pass to clang.  For values
      of param that are not relevant to code generation, returns None.
      """
      match = self.MACHINE_RE.match(param)
      if match and match.group(1).lower() in ['x86', 'i386', 'arm', '32']:
        return ['-m32']
      match = MLLVM_RE.match(param)
      if match:
        if match.group(2):
          return ['-mllvm', match.group(2)]
        else:
          return ['-mllvm']
      match = re.match('(?:-Wl,)?--lto-O(.*)', param)
      if match:
        optlevel[0] = match.group(1)
        return None
      match = re.match('[-/]opt:.*lldlto=([^:]*)', param, re.IGNORECASE)
      if match:
        optlevel[0] = match.group(1)
        return None
      if (param.startswith('-f') and not param.startswith('-flto')
          and not param.startswith('-fsanitize')
          and not param.startswith('-fthinlto')
          and not param.startswith('-fwhole-program')):
        return [param]
      return None

    def process_param(param):
      """
      Common code for processing a single parameter from the either the
      command line or an rsp file.
      """
      if in_mllvm[0]:
        if param.startswith('-Wl,'):
          codegen_params.append(param[4:])
        else:
          codegen_params.append(param)
        in_mllvm[0] = False
      else:
        cg_param = transform_codegen_param(param)
        if cg_param:
          codegen_params.extend(cg_param)
        match = MLLVM_RE.match(param)
        if match and not match.group(2):
          # Next parameter will be the thing to pass to LLVM.
          in_mllvm[0] = True
      if self.GROUP_RE.match(param):
        return
      index_params.append(param)
      if os.path.exists(param):
        match = self.OBJ_RE.match(param)
        if match and is_bitcode_file(param):
          native = obj_dir + '/' + match.group(1) + '.' + match.group(2)
          if use_common_objects:
            index = common_index
          else:
            index = obj_dir + '/' + param + '.thinlto.bc'
            create_file(index)
          codegen.append((os.path.normpath(native), param, index))
        else:
          final_params.append(param)
      elif not self.LTO_RE.match(param):
        final_params.append(param)

    index_params.append(self.WL + self.PREFIX_REPLACE + ';' + obj_dir)

    rsp_expanded = list(self.expand_args_rsps(args.linker_args))
    expanded_args = list(self.expand_thin_archives(rsp_expanded))
    i = 0
    while i < len(expanded_args):
      x = expanded_args[i]
      if not self.GROUP_RE.match(x):
        outfile, next_i = self.process_output_param(expanded_args, i)
        if outfile is not None:
          index_params.extend(expanded_args[i:next_i])
          final_params.extend(expanded_args[i:next_i])
          i = next_i - 1
        else:
          process_param(x)
      i += 1

    # If we are not doing ThinLTO codegen, just invoke the original command.
    if len(codegen) < 1:
      return None

    codegen_params.append('-O' + str(optlevel[0]))

    if use_common_objects:
      splitfile = None
      for tup in codegen:
        final_params.append(tup[0])
    else:
      splitfile = gen_dir + '/' + args.output + '.split' + self.OBJ_SUFFIX
      final_params.append(splitfile)
      index_params.append(self.WL + self.OBJ_PATH + splitfile)
      used_obj_file = gen_dir + '/' + args.output + '.objs'
      final_params.append('@' + used_obj_file)

    return AnalyzeArgsResult(
        output=args.output,
        linker=args.linker,
        compiler=compiler,
        splitfile=splitfile,
        index_params=index_params,
        codegen=codegen,
        codegen_params=codegen_params,
        final_params=final_params,
    )

  def gen_ninja(self, ninjaname, params, objs):
    """
    Generates a ninja build file at path ninjaname, using original command line
    params and with objs being a list of bitcode files for which to generate
    native code.
    """
    ensure_dir(os.path.dirname(ninjaname))
    with open(ninjaname, 'w') as f:
      f.write(('\nrule native-link\n  command = %s @$rspname'
               '\n  rspfile = $rspname\n  rspfile_content = $params\n') %
              (ninjaenc(params.linker), ))

      f.write(('\nrule codegen\n  command = %s %s -c %s'
               ' -fthinlto-index=$index %s$bitcode -o $out\n') %
              (ninjaenc(self.gomacc), ninjaenc(params.compiler),
               ninjajoin(params.codegen_params), self.XIR))

      for tup in params.codegen:
        obj, bitcode, index = tup
        f.write('\nbuild %s : codegen %s %s\n  bitcode = %s\n  index = %s\n' %
                tuple(map(ninjaenc, (obj, bitcode, index, bitcode, index))))

      f.write('\nbuild %s : native-link %s\n  rspname = %s\n  params = %s\n' %
              (ninjaenc(params.output), ninjajoin(objs),
               ninjaenc(params.output + '.final.rsp'),
               ninjajoin(params.final_params)))

      f.write('\ndefault %s\n' % (ninjaenc(params.output), ))

  def thin_link(self, params, gen_dir):
    """
    Performs the thin link step.
    This generates the index files, imports files, split LTO object,
    and used object file.
    Returns a list of native objects we need to generate from bitcode
    files for the final link step.
    """
    used_obj_file = gen_dir + '/' + params.output + '.objs'
    index_rsp = gen_dir + '/' + params.output + '.index.rsp'
    ensure_dir(gen_dir)
    ensure_dir(os.path.dirname(used_obj_file))
    if params.splitfile:
      ensure_dir(os.path.dirname(params.splitfile))
    self.write_rsp(index_rsp, params.index_params)
    index_cmd = [
        params.linker,
        self.WL + self.TLTO + '-index-only' + self.SEP + used_obj_file,
        self.WL + self.TLTO + '-emit-imports-files', '@' + index_rsp
    ]
    report_run(index_cmd)
    with open(used_obj_file) as f:
      codegen_objs = [
          os.path.normpath(x) for x in f.read().split('\n') if len(x) > 0
      ]
    return codegen_objs

  def codegen_and_link(self, params, gen_dir, objs):
    """
    Performs code generation for selected bitcode files and
    the final link step.
    objs should be the list of native object files expected to be generated
    (as returned by thin_link()).
    """
    ninjaname = gen_dir + '/build.ninja'
    self.gen_ninja(ninjaname, params, objs)
    cmd = [autoninja(), '-f', ninjaname]
    if self.jobs:
      cmd.extend(['-j', str(self.jobs)])
    report_run(cmd)

  def do_main(self, argv):
    """
    This function contains the main code to run. Not intended to be called
    directly. Call main instead, which returns exit status for failing
    subprocesses.
    """
    args = parse_args(argv)
    args.output = self.output_path(argv[1:])
    if args.output is None:
      return subprocess.call([args.linker] + args.linker_args)
    if args.gomacc:
      self.gomacc = args.gomacc
    if args.jobs:
      self.jobs = int(args.jobs)

    basename = os.path.basename(args.output)
    # Only generate tailored native object files for whitelisted targets.
    # TODO: Find a better way to structure this. There are three different
    # ways we can perform linking: Local ThinLTO, distributed ThinLTO,
    # and distributed ThinLTO with common object files.
    # We expect the distributed ThinLTO variants to be faster, but
    # common object files cannot be used when -fsplit-lto-unit is in effect.
    # Currently, we don't detect this situation. We could, but it might
    # be better to instead move this logic out of this script and into
    # the build system.
    use_common_objects = basename not in self.WHITELISTED_TARGETS
    common_dir = 'common_objs'
    gen_dir = 'lto.' + basename
    params = self.analyze_args(args, gen_dir, common_dir, use_common_objects)
    # If we determined that no distributed code generation need be done, just
    # invoke the original command.
    if params is None:
      return subprocess.call(argv[1:])
    if use_common_objects:
      objs = [x[0] for x in params.codegen]
      create_file(common_dir + '/empty.thinlto.bc')
    else:
      objs = self.thin_link(params, gen_dir)
    self.codegen_and_link(params, gen_dir, objs)
    return 0

  def main(self, argv):
    try:
      return self.do_main(argv)
    except subprocess.CalledProcessError as e:
      return e.returncode


class GomaLinkWindows(GomaLinkBase):
  # Target-platform-specific constants.
  WL = ''
  TLTO = '-thinlto'
  SEP = ':'
  GROUP_RE = re.compile(WL + '--(?:end|start)-group')
  MACHINE_RE = re.compile('[-/]machine:(.*)', re.IGNORECASE)
  OBJ_PATH = '-lto-obj-path' + SEP
  OBJ_SUFFIX = '.obj'
  OUTPUT_RE = re.compile('[-/]out:(.*)', re.IGNORECASE)
  PREFIX_REPLACE = TLTO + '-prefix-replace' + SEP
  XIR = ''

  WHITELISTED_TARGETS = {
      'chrome.exe',
      'chrome.dll',
      'chrome_child.dll',
      # TODO: The following targets have been whitelisted because the
      # common objects flow does not link them successfully. This should
      # be fixed, after which they can be removed from the whitelist.
      'tls_edit.exe',
  }

  def process_output_param(self, args, i):
    """
    If args[i] is a parameter that specifies the output file,
    returns (output_name, new_i). Else, returns (None, new_i).
    """
    m = self.OUTPUT_RE.match(args[i])
    if m:
      return (os.path.normpath(m.group(1)), i + 1)
    else:
      return (None, i + 1)


if __name__ == '__main__':
  sys.exit(GomaLinkWindows().main(sys.argv))
