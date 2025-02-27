# Copyright 2019 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import functools

from .argument import Argument
from .code_generator_info import CodeGeneratorInfo
from .composition_parts import WithCodeGeneratorInfo
from .composition_parts import WithComponent
from .composition_parts import WithDebugInfo
from .composition_parts import WithExposure
from .composition_parts import WithExtendedAttributes
from .composition_parts import WithOwner
from .composition_parts import WithOwnerMixin
from .exposure import Exposure
from .function_like import FunctionLike
from .function_like import OverloadGroup
from .idl_type import IdlType
from .make_copy import make_copy


class Operation(FunctionLike, WithExtendedAttributes, WithCodeGeneratorInfo,
                WithExposure, WithOwner, WithOwnerMixin, WithComponent,
                WithDebugInfo):
    """https://heycam.github.io/webidl/#idl-operations"""

    class IR(FunctionLike.IR, WithExtendedAttributes, WithCodeGeneratorInfo,
             WithExposure, WithOwnerMixin, WithComponent, WithDebugInfo):
        def __init__(self,
                     identifier,
                     arguments,
                     return_type,
                     is_static=False,
                     is_getter=False,
                     is_setter=False,
                     is_deleter=False,
                     extended_attributes=None,
                     component=None,
                     debug_info=None):
            assert isinstance(is_getter, bool)
            assert isinstance(is_setter, bool)
            assert isinstance(is_deleter, bool)
            assert is_getter + is_setter + is_deleter <= 1  # At most 1 True

            FunctionLike.IR.__init__(
                self,
                identifier=identifier,
                arguments=arguments,
                return_type=return_type,
                is_static=is_static)
            WithExtendedAttributes.__init__(self, extended_attributes)
            WithCodeGeneratorInfo.__init__(self)
            WithExposure.__init__(self)
            WithOwnerMixin.__init__(self)
            WithComponent.__init__(self, component)
            WithDebugInfo.__init__(self, debug_info)

            self.is_getter = is_getter
            self.is_setter = is_setter
            self.is_deleter = is_deleter
            self.is_stringifier = False

    def __init__(self, ir, owner):
        assert isinstance(ir, Operation.IR)

        FunctionLike.__init__(self, ir)
        WithExtendedAttributes.__init__(self, ir, readonly=True)
        WithCodeGeneratorInfo.__init__(self, ir, readonly=True)
        WithExposure.__init__(self, ir, readonly=True)
        WithOwner.__init__(self, owner)
        WithOwnerMixin.__init__(self, ir)
        WithComponent.__init__(self, ir, readonly=True)
        WithDebugInfo.__init__(self, ir)

        self._is_getter = ir.is_getter
        self._is_setter = ir.is_setter
        self._is_deleter = ir.is_deleter
        self._is_stringifier = ir.is_stringifier

    @property
    def is_special_operation(self):
        return (self.is_getter or self.is_setter or self.is_deleter
                or self.is_stringifier)

    @property
    def is_property_accessor(self):
        """
        Returns True if this is a indexed or named property accessor (one of
        getter, setter, or deleter).
        """
        return self.is_getter or self.is_setter or self.is_deleter

    @property
    def is_getter(self):
        return self._is_getter

    @property
    def is_setter(self):
        return self._is_setter

    @property
    def is_deleter(self):
        return self._is_deleter

    @property
    def is_stringifier(self):
        return self._is_stringifier


class OperationGroup(OverloadGroup, WithExtendedAttributes,
                     WithCodeGeneratorInfo, WithExposure, WithOwner,
                     WithComponent, WithDebugInfo):
    """
    Represents a group of operations with the same identifier.

    The number of operations in this group may be 1 or 2+.  In the latter case,
    the operations are overloaded.
    """

    class IR(OverloadGroup.IR, WithExtendedAttributes, WithCodeGeneratorInfo,
             WithExposure, WithDebugInfo):
        def __init__(self,
                     operations,
                     extended_attributes=None,
                     code_generator_info=None,
                     debug_info=None):
            OverloadGroup.IR.__init__(self, operations)
            WithExtendedAttributes.__init__(self, extended_attributes)
            WithCodeGeneratorInfo.__init__(self, code_generator_info)
            WithExposure.__init__(self)
            WithDebugInfo.__init__(self, debug_info)

    def __init__(self, ir, operations, owner):
        assert isinstance(ir, OperationGroup.IR)
        assert isinstance(operations, (list, tuple))
        assert all(
            isinstance(operation, Operation) for operation in operations)
        assert all(
            operation.identifier == ir.identifier for operation in operations)

        components = functools.reduce(
            lambda s, operation: s.union(operation.components), operations,
            set())

        ir = make_copy(ir)
        OverloadGroup.__init__(self, functions=operations)
        WithExtendedAttributes.__init__(self, ir, readonly=True)
        WithCodeGeneratorInfo.__init__(self, ir, readonly=True)
        WithExposure.__init__(self, ir, readonly=True)
        WithOwner.__init__(self, owner)
        WithComponent.__init__(self, sorted(components))
        WithDebugInfo.__init__(self, ir)
