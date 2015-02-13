from SCons.Script import *
from SCons.Util import splitext


########################################################################
#
#  run our analysis plugin and collect results
#


def __run_plugin_emitter(target, source, env):
    source.insert(0, '$plugin')
    return target, source


def __run_plugin_source_args(target, source, env, for_signature):
    def generate():
        overreport = True
        for input in source:
            extension = splitext(input.name)[1]
            if extension == '.json':
                overreport = False
                yield '-iiglue-read-file'
                yield input
            elif extension == '.so':
                yield '-load'
                yield './%s' % input
            else:
                yield input
        if overreport:
            yield '-overreport'
    return list(generate())


__run_plugin_builder = Builder(
    action='/scratch/ajmaas/SymbolicRange/install/bin/opt -o $TARGET ' + 
    #'-load /p/polyglot/public/tools/range-analysis/install/lib/vSSA.so ' + 
    #'-load /p/polyglot/public/tools/range-analysis/install/lib/RAInstrumentation.so ' +
    #'-load /p/polyglot/public/tools/range-analysis/install/lib/RangeAnalysis.so ' + 
    #'-load /p/polyglot/public/tools/range-analysis/install/lib/RAPrinter.so ' +
    '-load /scratch/ajmaas/SymbolicRange/install/lib/vSSA.so ' + 
    '-load /scratch/ajmaas/SymbolicRange/install/lib/RAInstrumentation.so ' +
    '-load /scratch/ajmaas/SymbolicRange/install/lib/RangeAnalysis.so ' + 
    '-load /scratch/ajmaas/SymbolicRange/install/lib/RAPrinter.so ' +
    '-load /scratch/ajmaas/SymbolicRange/install/lib/GreenArrays.so ' +
    '$_RUN_PLUGIN_SOURCE_ARGS $PLUGIN_ARGS',
    emitter=__run_plugin_emitter,
    suffix='.actual',
)


########################################################################


def generate(env):
    if 'RunPlugin' in env['BUILDERS']:
        return

    env.AppendUnique(
        BUILDERS={
            'RunPlugin': __run_plugin_builder,
        },
        _RUN_PLUGIN_SOURCE_ARGS=__run_plugin_source_args,
    )


def exists(env):
    return true
