# ==============================================================================
# Copyright 2017 NEWPLAN, Tsinghua University. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
import os
from distutils.errors import CompileError, DistutilsError, DistutilsPlatformError, LinkError
from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext
import shlex
import subprocess
import textwrap
import traceback

from bcube import __version__
#This version is 0.1

tensorflow_bcube_lib = Extension('bcube.tensorflow.bcube_lib', [])


def check_tf_version():
    try:
        import tensorflow as tf
        if tf.__version__ < '1.1.0':
            raise DistutilsPlatformError(
                'Your TensorFlow version %s is outdated.  '
                'Bcube requires tensorflow>=1.1.0' % tf.__version__)
    except ImportError:
        raise DistutilsPlatformError(
            'import tensorflow failed, is it installed?\n\n%s' % traceback.format_exc())
    except AttributeError:
        # This means that tf.__version__ was not exposed, which makes it *REALLY* old.
        raise DistutilsPlatformError(
            'Your TensorFlow version is outdated.  Bcube requires tensorflow>=1.1.0')


def get_tf_include_dirs():
    import tensorflow as tf
    tf_inc = tf.sysconfig.get_include()
    return [tf_inc, '%s/external/nsync/public' % tf_inc]


def get_tf_lib_dirs():
    import tensorflow as tf
    tf_lib = tf.sysconfig.get_lib()
    return [tf_lib]


def get_tf_libs(build_ext, lib_dirs):
    last_err = None
    for tf_libs in [['tensorflow_framework'], []]:
        try:
            lib_file = test_compile(build_ext, 'test_tensorflow_libs',
                                    library_dirs=lib_dirs, libraries=tf_libs,
                                    code=textwrap.dedent('''\
                    void test() {
                    }
                    '''))

            from tensorflow.python.framework import load_library
            load_library.load_op_library(lib_file)

            return tf_libs
        except (CompileError, LinkError):
            last_err = 'Unable to determine -l link flags to use with TensorFlow (see error above).'
        except Exception:
            last_err = 'Unable to determine -l link flags to use with TensorFlow.  ' \
                       'Last error:\n\n%s' % traceback.format_exc()

    raise DistutilsPlatformError(last_err)


def get_tf_abi(build_ext, include_dirs, lib_dirs, libs):
    last_err = None
    cxx11_abi_macro = '_GLIBCXX_USE_CXX11_ABI'
    for cxx11_abi in ['0', '1']:
        try:
            lib_file = test_compile(build_ext, 'test_tensorflow_abi',
                                    macros=[(cxx11_abi_macro, cxx11_abi)],
                                    include_dirs=include_dirs, library_dirs=lib_dirs,
                                    libraries=libs, code=textwrap.dedent('''\
                #include <string>
                #include "tensorflow/core/framework/op.h"
                #include "tensorflow/core/framework/op_kernel.h"
                #include "tensorflow/core/framework/shape_inference.h"
                void test() {
                    auto ignore = tensorflow::strings::StrCat("a", "b");
                }
                '''))

            from tensorflow.python.framework import load_library
            load_library.load_op_library(lib_file)

            return cxx11_abi_macro, cxx11_abi
        except (CompileError, LinkError):
            last_err = 'Unable to determine CXX11 ABI to use with TensorFlow (see error above).'
        except Exception:
            last_err = 'Unable to determine CXX11 ABI to use with TensorFlow.  ' \
                       'Last error:\n\n%s' % traceback.format_exc()

    raise DistutilsPlatformError(last_err)


def get_tf_flags(build_ext):
    import tensorflow as tf
    try:
        return tf.sysconfig.get_compile_flags(), tf.sysconfig.get_link_flags()
    except AttributeError:
        # fallback to the previous logic
        tf_include_dirs = get_tf_include_dirs()
        tf_lib_dirs = get_tf_lib_dirs()
        tf_libs = get_tf_libs(build_ext, tf_lib_dirs)
        tf_abi = get_tf_abi(build_ext, tf_include_dirs, tf_lib_dirs, tf_libs)

        compile_flags = []
        for include_dir in tf_include_dirs:
            compile_flags.append('-I%s' % include_dir)
        if tf_abi:
            compile_flags.append('-D%s=%s' % tf_abi)

        link_flags = []
        for lib_dir in tf_lib_dirs:
            link_flags.append('-L%s' % lib_dir)
        for lib in tf_libs:
            link_flags.append('-l%s' % lib)

        return compile_flags, link_flags


def test_compile(build_ext, name, code, libraries=None, include_dirs=None, library_dirs=None, macros=None):
    test_compile_dir = os.path.join(build_ext.build_temp, 'test_compile')
    if not os.path.exists(test_compile_dir):
        os.makedirs(test_compile_dir)

    source_file = os.path.join(test_compile_dir, '%s.cc' % name)
    with open(source_file, 'w') as f:
        f.write(code)

    compiler = build_ext.compiler
    [object_file] = compiler.object_filenames([source_file])
    shared_object_file = compiler.shared_object_filename(
        name, output_dir=test_compile_dir)

    compiler.compile([source_file], extra_preargs=['-std=c++11'],
                     include_dirs=include_dirs, macros=macros)
    compiler.link_shared_object(
        [object_file], shared_object_file, libraries=libraries, library_dirs=library_dirs)

    return shared_object_file


def get_cuda_dirs(build_ext):
    cuda_include_dirs = []
    cuda_lib_dirs = []

    cuda_home = os.environ.get('BCUBE_CUDA_HOME')
    if cuda_home:
        cuda_include_dirs += ['%s/include' % cuda_home]
        cuda_lib_dirs += ['%s/lib' % cuda_home, '%s/lib64' % cuda_home]

    cuda_include = os.environ.get('BCUBE_CUDA_INCLUDE')
    if cuda_include:
        cuda_include_dirs += [cuda_include]

    cuda_lib = os.environ.get('BCUBE_CUDA_LIB')
    if cuda_lib:
        cuda_lib_dirs += [cuda_lib]

    if not cuda_include_dirs and not cuda_lib_dirs:
        # default to /usr/local/cuda
        cuda_include_dirs += ['/usr/local/cuda/include']
        cuda_lib_dirs += ['/usr/local/cuda/lib', '/usr/local/cuda/lib64']

    try:
        test_compile(build_ext, 'test_cuda', libraries=['cudart'], include_dirs=cuda_include_dirs,
                     library_dirs=cuda_lib_dirs, code=textwrap.dedent('''\
            #include <cuda_runtime.h>
            void test() {
                cudaSetDevice(0);
            }
            '''))
    except (CompileError, LinkError):
        raise DistutilsPlatformError(
            'CUDA library was not found (see error above).\n'
            'Please specify correct CUDA location with the BCUBE_CUDA_HOME '
            'environment variable or combination of BCUBE_CUDA_INCLUDE and '
            'BCUBE_CUDA_LIB environment variables.\n\n'
            'BCUBE_CUDA_HOME - path where CUDA include and lib directories can be found\n'
            'BCUBE_CUDA_INCLUDE - path to CUDA include directory\n'
            'BCUBE_CUDA_LIB - path to CUDA lib directory')

    return cuda_include_dirs, cuda_lib_dirs


def get_rdma_dirs(build_ext, cuda_include_dirs, cuda_lib_dirs):
    rdma_include_dirs = []
    rdma_lib_dirs = []
    return rdma_include_dirs, rdma_lib_dirs


def fully_define_extension(build_ext):
    check_tf_version()

    tf_compile_flags, tf_link_flags = get_tf_flags(build_ext)

    gpu_allreduce = os.environ.get('BCUBE_GPU_ALLREDUCE')
    if gpu_allreduce and gpu_allreduce != 'TCP' and gpu_allreduce != 'RDMA':
        raise DistutilsError('BCUBE_GPU_ALLREDUCE=%s is invalid, supported '
                             'values are "", "TCP", "RDMA".' % gpu_allreduce)

    gpu_allgather = os.environ.get('BCUBE_GPU_ALLGATHER')
    if gpu_allgather and gpu_allgather != 'TCP' and gpu_allgather != 'RDMA':
        raise DistutilsError('BCUBE_GPU_ALLGATHER=%s is invalid, supported '
                             'values are "", "TCP", "RDMA".' % gpu_allgather)

    gpu_broadcast = os.environ.get('BCBUE_GPU_BROADCAST')
    if gpu_broadcast and gpu_broadcast != 'TCP'and gpu_broadcast != 'RDMA':
        raise DistutilsError('BCUBE_GPU_BROADCAST=%s is invalid, supported '
                             'values are "", "TCP", "RDMA".' % gpu_broadcast)

    if gpu_allreduce or gpu_allgather or gpu_broadcast:
        have_cuda = True
        cuda_include_dirs, cuda_lib_dirs = get_cuda_dirs(build_ext)
    else:
        have_cuda = False
        cuda_include_dirs = cuda_lib_dirs = []

    if gpu_allreduce == 'RDMA':
        have_rdma = True
        rdma_include_dirs, rdma_lib_dirs = get_rdma_dirs(
            build_ext, cuda_include_dirs, cuda_lib_dirs)
    else:
        have_rdma = False
        rdma_include_dirs = rdma_lib_dirs = []

    MACROS = []
    INCLUDES = []
    SOURCES = ['bcube/tensorflow/bcube_message.cpp',
				'bcube/tensorflow/bcube_utils.cpp',
               'bcube/tensorflow/bcube_ops.cpp',
               'bcube/tensorflow/bcube_comm.cpp']
    #COMPILE_FLAGS = ['-std=c++11', '-fPIC', '-Os'] + tf_compile_flags
    COMPILE_FLAGS = ['-std=c++11'] + tf_compile_flags
    LINK_FLAGS = tf_link_flags
    LIBRARY_DIRS = []
    LIBRARIES = []

    if have_cuda:
        MACROS += [('HAVE_CUDA', '1')]
        INCLUDES += cuda_include_dirs
        LIBRARY_DIRS += cuda_lib_dirs
        LIBRARIES += ['cudart']

    if have_rdma:
        MACROS += [('HAVE_rdma', '1')]
        INCLUDES += nccl_include_dirs
        LIBRARY_DIRS += nccl_lib_dirs
        LIBRARIES += ['rdma']

    if gpu_allreduce:
        MACROS += [('BCUBE_GPU_ALLREDUCE', "'%s'" % gpu_allreduce[0])]

    if gpu_allgather:
        MACROS += [('BCUBE_GPU_ALLGATHER', "'%s'" % gpu_allgather[0])]

    if gpu_broadcast:
        MACROS += [('BCUBE_GPU_BROADCAST', "'%s'" % gpu_broadcast[0])]

    tensorflow_bcube_lib.define_macros = MACROS
    tensorflow_bcube_lib.include_dirs = INCLUDES
    tensorflow_bcube_lib.sources = SOURCES
    tensorflow_bcube_lib.extra_compile_args = COMPILE_FLAGS
    tensorflow_bcube_lib.extra_link_args = LINK_FLAGS
    tensorflow_bcube_lib.library_dirs = LIBRARY_DIRS
    tensorflow_bcube_lib.libraries = LIBRARIES

    print(tensorflow_bcube_lib.define_macros)
    print(tensorflow_bcube_lib.include_dirs)
    print(tensorflow_bcube_lib.sources)
    print(tensorflow_bcube_lib.extra_compile_args)
    #print(tensorflow_bcube_lib.compile_args)
    print(tensorflow_bcube_lib.extra_link_args)
    print(tensorflow_bcube_lib.library_dirs)
    print(tensorflow_bcube_lib.libraries)


# run the customize_compiler
class custom_build_ext(build_ext):
    def build_extensions(self):
        fully_define_extension(self)
        build_ext.build_extensions(self)


setup(name='bcube',
      version=__version__,
      packages=find_packages(),
      description='Distributed training framework for TensorFlow.',
      author='NEWPLAN @ Tsinghua University.',
      long_description=textwrap.dedent('''\
          Bcube is a distributed training framework for TensorFlow. 
          The goal of Bcube is to make distributed Deep Learning
          fast and easy to use.'''),
      url='https://nasp.cs.tsinghua.edu.cn',
      classifiers=[
          'License :: OSI Approved :: Apache Software License'
      ],
      ext_modules=[tensorflow_bcube_lib],
      cmdclass={'build_ext': custom_build_ext},
      zip_safe=False)
