"""Rebuilds the line stack of NS_LBA_Airflow (the Dinofly's wind lines, see ULBAAirflowComponent).

Run it in a running editor through the Unreal MCP server (Epic's ModelContextProtocol plugin, enabled in this project):
pass this file's text as the `script` of the editor_toolset ProgrammaticToolset.execute_tool_script tool, which provides
`execute_tool` and calls `run()`. It removes the Strands emitter's Set Parameters modules and user parameters (except
User.TrailMaterial), adds them back as below and saves the asset; the emitter itself (spawn per frame, no interpolated
spawning, ribbon renderer with its id bound to Particles.Streak, M_LBA_Airflow) is kept.

Each line is one ribbon of User.Points particles spawned together; every value is a closed-form HLSL expression of the
particle's age, so the stack has no forces or solvers. The component places each line and sets the User parameters: what
a line needs is copied to its particles when it starts, so a system instance can start another line while one finishes.
"""
import json
S = {'refPath': '/LBA2Importer/Effects/NS_LBA_Airflow.NS_LBA_Airflow'}
TY = {'pos': '/Script/Niagara.NiagaraPosition', 'float': '/Script/Niagara.NiagaraFloat', 'int': '/Script/Niagara.NiagaraInt32',
      'vec': '/Script/CoreUObject.Vector3f', 'color': '/Script/CoreUObject.LinearColor'}
ZERO = {'pos': {'x': 0, 'y': 0, 'z': 0}, 'float': {'value': 0}, 'int': {'value': 0}, 'vec': {'x': 0, 'y': 0, 'z': 0},
        'color': {'r': 1, 'g': 1, 'b': 1, 'a': 1}}


def T(name, args):
    return execute_tool('NiagaraToolsets.NiagaraToolset_System.' + name, json.dumps(args))


def J(x):
    return json.loads(json.dumps(x))


def ref(script='', module='', inputs=None, renderer=-1):
    return {'system': S, 'emitterName': 'Strands', 'scriptName': script, 'moduleName': module, 'rendererIndex': renderer,
            'inputNameStack': inputs or []}


def typ(t):
    return {'classStructOrEnum': {'refPath': TY[t]}}


def value(t, v):
    if t in ('float', 'int'):
        return {'struct': {'refPath': TY[t]}, 'value': {'value': v}}
    if t == 'color':
        return {'struct': {'refPath': TY[t]}, 'value': {'r': v[0], 'g': v[1], 'b': v[2], 'a': v[3]}}
    return {'struct': {'refPath': TY[t]}, 'value': {'x': v[0], 'y': v[1], 'z': v[2]}}


def expr(e):
    return {'struct': {'refPath': '/Script/NiagaraEditor.NiagaraExt_StackInputData_HlslExpression'}, 'value': {'hlslExpression': e}}


def link(name, t):
    return {'struct': {'refPath': '/Script/NiagaraEditor.NiagaraExt_StackInputData_Linked'},
            'value': {'linkedVariable': {'name': name, 'type': typ(t)}}}


def set_params(script, entries):
    r = J(T('AddSetParametersModule', {'moduleLocationRef': ref(script), 'parameters': [
        {'variable': {'name': n, 'type': typ(t)}, 'defaultValue': {'struct': {'refPath': TY[t]}, 'value': ZERO[t]}}
        for n, t, e in entries]})['returnValue'])
    module = r['moduleName']
    for n, t, e in entries:
        T('SetStackInputData', {'stackInputRef': ref(script, module, [n]), 'inputData': expr(e)})
    return module


USER = [
    ('SpawnCount', 'int', 0, 'Ribbon points spawned this frame: new lines x Points.'),
    ('Points', 'int', 32, 'Ribbon points per line.'),
    ('Base', 'pos', (0, 0, 0), 'Where a new line starts (set when it starts).'),
    ('HeadVelocity', 'vec', (-1000, 0, 0), 'Velocity (cm/s) of a new line head along its path.'),
    ('Bend', 'vec', (0, 0, 0), 'Acceleration (cm/s2) of a new line head along its path: its bow and the body path curve.'),
    ('Drift', 'vec', (0, 0, 0), 'Wind (cm/s) carrying a new line.'),
    ('Across', 'vec', (0, 1, 0), 'Across a new line, facing the camera: which way it bows and curls.'),
    ('Life', 'float', 1, 'Seconds from the birth of a new line to its tail reaching its head.'),
    ('Lag', 'float', 0.4, 'Seconds the tail of a new line lags behind its head.'),
    ('HookRadius', 'float', 0, 'Radius (cm) of the curl at the end of a new line (0 = none).'),
    ('HookTime', 'float', 0, 'Seconds the head of a new line takes through its curl.'),
    ('HookTurn', 'float', 4, 'Radians a curl turns.'),
    ('Visibility', 'float', 1, 'Opacity factor of new lines (grows with air speed).'),
    ('Width', 'float', 6, 'Width (cm) of a new line at its thickest.'),
    ('FadeIn', 'float', 0.08, 'Seconds a new line takes to fade in.'),
    ('Color', 'color', (1, 1, 1, 1), 'Line color, alpha = opacity.'),
    ('TailOpacity', 'float', 0.5, 'Opacity of a line tail, against its head.'),
    ('MinPixels', 'float', 1.5, 'Lines are at least this wide on screen; thinner ones fade instead.'),
    ('PixelAngle', 'float', 0.001, 'Size of a pixel at 1 cm from the camera (set each frame).'),
    ('MaxAngle', 'float', 0.008, 'Widest a line gets on screen, at 1 cm from the camera (set each frame).'),
    ('CameraPosition', 'pos', (0, 0, 0), 'Camera location (set each frame).'),
    ('NearFade', 'float', 300, 'Lines fade out closer than this to the camera (cm).'),
]

KF = 'float(max(User.Points, 2))'
STREAK = '(floor((float(Particles.UniqueID) + 0.5) / ' + KF + '))'

# A line is the trail of its head: the head runs along a path through the air, every ribbon point (U = 0 head .. 1 tail)
# lies on that same path at path time Tau, so the tail follows the head through its bow and curl. The path is closed form:
# a straight run bent by Bend, then for some lines a curl tightening to its end, in the plane across the line that faces
# the camera. The wind (Drift) carries the whole line. The head runs until TEnd, the tail Lag behind it: the line grows
# from nothing, slides, then shrinks.
SPAWN = [
    [
        ('Particles.Streak', 'int', 'int(' + STREAK + ')'),
        ('Particles.U', 'float', 'fmod(float(Particles.UniqueID), ' + KF + ') / (' + KF + ' - 1.0)'),
    ],
    [
        # The component gives each line its life (it counts the lines on screen).
        ('Particles.Lifetime', 'float', 'User.Life'),
        ('Particles.Base', 'pos', 'User.Base'),
        ('Particles.HeadVelocity', 'vec', 'User.HeadVelocity'),
        ('Particles.Bend', 'vec', 'User.Bend'),
        ('Particles.Drift', 'vec', 'User.Drift'),
        ('Particles.Across', 'vec', 'User.Across'),
        ('Particles.Lag', 'float', 'max(User.Lag, 0.001)'),
        ('Particles.HookRadius', 'float', 'User.HookRadius'),
        ('Particles.HookTime', 'float', 'User.HookTime'),
        ('Particles.HookTurn', 'float', 'User.HookTurn'),
        ('Particles.Visibility', 'float', 'User.Visibility'),
        ('Particles.Width', 'float', 'User.Width'),
        ('Particles.FadeIn', 'float', 'max(User.FadeIn, 0.001)'),
        ('Particles.RibbonLinkOrder', 'float', 'Particles.U'),
    ],
    [
        ('Particles.TEnd', 'float', 'Particles.Lifetime - Particles.Lag'),
    ],
    [
        ('Particles.HookStart', 'float', 'Particles.TEnd - Particles.HookTime'),
    ],
    [
        # The way the head goes when it enters the curl, and the side it curls to, square to that way.
        ('Particles.HookDir', 'vec', '(Particles.HeadVelocity + Particles.Bend * Particles.HookStart) / max(length(Particles.HeadVelocity + Particles.Bend * Particles.HookStart), 0.001)'),
    ],
    [
        ('Particles.HookN', 'vec', '(Particles.Across - dot(Particles.Across, Particles.HookDir) * Particles.HookDir) '
         '/ max(length(Particles.Across - dot(Particles.Across, Particles.HookDir) * Particles.HookDir), 0.0001)'),
    ],
]
UPDATE = [
    [
        ('Particles.TauHead', 'float', 'min(Particles.Age, Particles.TEnd)'),
        ('Particles.TauTail', 'float', 'max(Particles.Age - Particles.Lag, 0.0)'),
    ],
    [
        ('Particles.Tau', 'float', 'lerp(Particles.TauHead, Particles.TauTail, Particles.U)'),
        ('Particles.Live', 'float', 'saturate(3.0 * (Particles.TauHead - Particles.TauTail) / Particles.Lag)'),
    ],
    [
        ('Particles.HookX', 'float', 'saturate((Particles.Tau - Particles.HookStart) / max(Particles.HookTime, 0.001))'),
        ('Particles.TauRun', 'float', 'min(Particles.Tau, Particles.HookStart)'),
        # Sharp at both ends, thickest a bit behind the head.
        ('Particles.Profile', 'float', 'pow(max(sin(3.14159265 * pow(max(Particles.U, 0.0001), 0.8)), 0.0001), 0.7) * Particles.Live'),
    ],
    [
        ('Particles.HookPhi', 'float', 'Particles.HookTurn * Particles.HookX'),
    ],
    [
        ('Particles.Position', 'pos', 'Particles.Base + Particles.Drift * Particles.Age + Particles.HeadVelocity * Particles.TauRun '
         '+ 0.5 * Particles.Bend * Particles.TauRun * Particles.TauRun '
         '+ Particles.HookRadius * (1.0 - 0.45 * Particles.HookX) * (sin(Particles.HookPhi) * Particles.HookDir + (1.0 - cos(Particles.HookPhi)) * Particles.HookN)'),
    ],
    [
        ('Particles.PixelWidth', 'float', 'User.MinPixels * User.PixelAngle * length(Particles.Position - User.CameraPosition)'),
        ('Particles.LineWidth', 'float', 'min(Particles.Width, User.MaxAngle * length(Particles.Position - User.CameraPosition)) * Particles.Profile'),
    ],
    [
        ('Particles.RibbonWidth', 'float', 'max(Particles.LineWidth, Particles.PixelWidth)'),
        ('Particles.Color', 'color', 'float4(User.Color.rgb, User.Color.a * Particles.Visibility * lerp(1.0, User.TailOpacity, Particles.U) * saturate(Particles.LineWidth / max(Particles.PixelWidth, 0.001)) '
         '* saturate(2.0 * length(Particles.Position - User.CameraPosition) / max(User.NearFade, 1.0) - 1.0) * saturate(Particles.Age / Particles.FadeIn))'),
    ],
]


def run():
    out = {}
    topo = J(T('GetEmitterTopology', {'emitterRef': ref()})['returnValue'])
    removed = []
    for key, script in (('particleSpawnScript', 'ParticleSpawnScript'), ('particleUpdateScript', 'ParticleUpdateScript')):
        for m in topo[key]['modules']:
            if m['moduleName'].startswith('SetVariables'):
                T('RemoveModule', {'moduleToRemove': ref(script, m['moduleName'])})
                removed.append(m['moduleName'])
    out['removed'] = removed
    old = J(T('GetUserVariables', {'system': S})['returnValue'])['userVariables']
    remove = [{'name': v['name'], 'type': v['type']} for v in old if v['name'] != 'User.TrailMaterial']
    T('RemoveUserVariables', {'system': S, 'variablesToRemove': remove})
    T('AddUserVariables', {'system': S, 'variablesToAdd': [
        {'name': 'User.' + n, 'type': typ(t), 'defaultValue': value(t, d), 'description': desc} for n, t, d, desc in USER]})
    T('SetStackInputData', {'stackInputRef': ref('EmitterUpdateScript', 'SpawnPerFrame', ['Spawn Count']), 'inputData': link('User.SpawnCount', 'int')})
    T('SetStackInputData', {'stackInputRef': ref('ParticleSpawnScript', 'InitializeParticle', ['Lifetime']), 'inputData': link('User.Life', 'float')})
    out['spawn'] = [set_params('ParticleSpawnScript', m) for m in SPAWN]
    out['update'] = [set_params('ParticleUpdateScript', m) for m in UPDATE]
    # Compiling is asynchronous and this call waits for it: saved any sooner, the asset lacks the last modules' compile.
    out['compile'] = J(T('GetSystemCompileState', {'system': S})['returnValue'])['aggregateStatus']
    out['saved'] = execute_tool('editor_toolset.toolsets.asset.AssetTools.save_assets', json.dumps({'asset_paths': ['/LBA2Importer/Effects/NS_LBA_Airflow']}))['returnValue']
    return out
