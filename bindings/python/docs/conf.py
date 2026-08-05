project = 'zlink Python Binding'
extensions = ['sphinx.ext.autodoc', 'sphinx.ext.viewcode']
autodoc_member_order = 'bysource'
autodoc_mock_imports = ['zlink._ffi', 'zlink._native', 'zlink.native']

import os, sys
sys.path.insert(0, os.path.abspath('../src'))

html_theme = 'alabaster'
exclude_patterns = ['_build']
