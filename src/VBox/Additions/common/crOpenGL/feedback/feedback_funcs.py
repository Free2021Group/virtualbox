# Copyright (c) 2001, Stanford University
# All rights reserved.
#
# See the file LICENSE.txt for information on redistributing this software.

from __future__ import print_function
import sys

import apiutil


apiutil.CopyrightC()

print("""
/* DO NOT EDIT - THIS FILE AUTOMATICALLY GENERATED BY feedback_funcs.py SCRIPT */
#ifndef CR_STATE_FEEDBACK_FUNCS_H
#define CR_STATE_FEEDBACK_FUNCS_H

#include "cr_error.h"

#if defined(WINDOWS)
#define STATE_APIENTRY __stdcall
#else
#define STATE_APIENTRY
#endif

#define STATE_UNUSED(x) ((void)x)""")

keys = apiutil.GetDispatchedFunctions(sys.argv[1]+"/APIspec.txt")

for func_name in apiutil.AllSpecials( "feedback" ):
	return_type = apiutil.ReturnType(func_name)
	params = apiutil.Parameters(func_name)
	print('%s STATE_APIENTRY crStateFeedback%s(%s);' % (return_type, func_name, apiutil.MakeDeclarationString(params)))

for func_name in apiutil.AllSpecials( "select" ):
	return_type = apiutil.ReturnType(func_name)
	params = apiutil.Parameters(func_name)
	print('%s STATE_APIENTRY crStateSelect%s(%s);' % (return_type, func_name, apiutil.MakeDeclarationString(params)))
print('\n#endif /* CR_STATE_FEEDBACK_FUNCS_H */')
