from mongrel2.config.model import *

def include(name, script):
    import imp
    imp.load_source('mongrel2_config_' + name, script)

