"""Makes NS_LBA_Rain's drops follow User.DropVelocity every frame (not only when spawned).

The view recycler moves old drops back into view without respawning them, so a velocity set at spawn would stay
forever; ULBAWindSubsystem changes User.DropVelocity as the storm's wind changes. Run it in a running editor through the
Unreal MCP server: pass this file's text as the `script` of editor_toolset ProgrammaticToolset.execute_tool_script
(`execute_tool` is provided, `run()` is called). It adds one Set Parameters module to the Drops emitter's particle
update (Particles.Velocity = User.DropVelocity), unless one is there already, and saves the asset.
"""
import json
S = {'refPath': '/LBA2Importer/Effects/NS_LBA_Rain.NS_LBA_Rain'}
VEC = '/Script/CoreUObject.Vector3f'


def T(name, args):
    return execute_tool('NiagaraToolsets.NiagaraToolset_System.' + name, json.dumps(args))


def ref(script='', module='', inputs=None):
    return {'system': S, 'emitterName': 'Drops', 'scriptName': script, 'moduleName': module, 'rendererIndex': -1,
            'inputNameStack': inputs or []}


def run():
    out = {}
    topo = json.loads(json.dumps(T('GetEmitterTopology', {'emitterRef': ref()})['returnValue']))
    update = [m['moduleName'] for m in topo['particleUpdateScript']['modules']]
    for name in update:
        if name.startswith('SetVariables'):
            T('RemoveModule', {'moduleToRemove': ref('ParticleUpdateScript', name)})
    vtype = {'classStructOrEnum': {'refPath': VEC}}
    r = json.loads(json.dumps(T('AddSetParametersModule', {'moduleLocationRef': ref('ParticleUpdateScript'), 'parameters': [
        {'variable': {'name': 'Particles.Velocity', 'type': vtype},
         'defaultValue': {'struct': {'refPath': VEC}, 'value': {'x': 0, 'y': 0, 'z': 0}}}]})['returnValue']))
    module = r['moduleName']
    T('SetStackInputData', {'stackInputRef': ref('ParticleUpdateScript', module, ['Particles.Velocity']),
                            'inputData': {'struct': {'refPath': '/Script/NiagaraEditor.NiagaraExt_StackInputData_Linked'},
                                          'value': {'linkedVariable': {'name': 'User.DropVelocity', 'type': vtype}}}})
    out['module'] = module
    topo = json.loads(json.dumps(T('GetEmitterTopology', {'emitterRef': ref()})['returnValue']))
    out['update'] = [m['moduleName'] for m in topo['particleUpdateScript']['modules']]
    out['saved'] = execute_tool('editor_toolset.toolsets.asset.AssetTools.save_assets',
                                json.dumps({'asset_paths': ['/LBA2Importer/Effects/NS_LBA_Rain']}))['returnValue']
    return out
