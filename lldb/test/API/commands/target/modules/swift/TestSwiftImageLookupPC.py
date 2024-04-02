import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil
import unittest2


class SwiftAddressExpressionTest(TestBase):
    @swiftTest
    @skipIfLinux # rdar://125497260
    def test(self):
        """Test that you can use register names in image lookup in a swift frame."""
        self.build()
        (target, process, thread, breakpoint) = lldbutil.run_to_source_breakpoint(self, 
            "break here to check image lookup", lldb.SBFileSpec("main.swift"))
        # I don't want to be too specific in what we print for image lookup,
        # we're testing that the address expression for the pc worked.
        self.expect("image lookup -va $pc", substrs=["doSomething"])
        self.expect("image lookup -va $pc+4", substrs=["doSomething"])

    def test_using_alias(self):
        """Test that you can use register names in image lookup in a swift frame."""
        self.build()
        (target, process, thread, breakpoint) = lldbutil.run_to_source_breakpoint(self, 
            "break here to check image lookup", lldb.SBFileSpec("main.swift"))
        # I don't want to be too specific in what we print for image lookup,
        # we're testing that the address expression for the pc worked.
        self.expect("target modules lookup -va $pc", substrs=["doSomething"])
        self.expect("target modules lookup -va $pc+4", substrs=["doSomething"])
        
    def test_using_separate_options(self):
        """Test that you can use register names in image lookup in a swift frame."""
        self.build()
        (target, process, thread, breakpoint) = lldbutil.run_to_source_breakpoint(self, 
            "break here to check image lookup", lldb.SBFileSpec("main.swift"))
        # I don't want to be too specific in what we print for image lookup,
        # we're testing that the address expression for the pc worked.
        self.expect("target modules lookup -v -a $pc", substrs=["doSomething"])
        self.expect("target modules lookup -v -a $pc+4", substrs=["doSomething"])
        
