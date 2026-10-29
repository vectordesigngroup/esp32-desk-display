Import("env")

import os

setup = os.path.join(env["PROJECT_DIR"], "include", "display_setup.h").replace("\\", "/")
env.Append(CCFLAGS=["-include", setup])
