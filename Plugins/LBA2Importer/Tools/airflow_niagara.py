"""Rebuilds the stroke stack of NS_LBA_Airflow (the Dinofly's airflow strokes, see ULBAAirflowComponent).

Run it in a running editor through the Unreal MCP server (Epic's ModelContextProtocol plugin, enabled in this project):
pass this file's text as the `script` of the editor_toolset ProgrammaticToolset.execute_tool_script tool, which provides
`execute_tool` and calls `run()`. It removes the Strands emitter's Set Parameters modules and user parameters (except
User.TrailMaterial), adds them back as below and saves the asset; the emitter itself (spawn per frame, no interpolated
spawning, ribbon renderer with its id bound to Particles.Streak, M_LBA_Airflow) is kept.

Each stroke is one ribbon of User.Points particles spawned together; every value is a closed-form HLSL expression of the
particle's age, so the stack has no forces or solvers. The component sets the User parameters each frame.
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
    ('SpawnCount', 'int', 0, 'Ribbon points spawned this frame: new strokes x Points.'),
    ('Points', 'int', 20, 'Ribbon points per stroke.'),
    ('Extent', 'vec', (0, 0, 0), 'Half the body part segment (cm): strokes start anywhere along it.'),
    ('Spread', 'float', 30, 'Radius (cm) around the segment where strokes start.'),
    ('Perp1', 'vec', (0, 1, 0), 'Across the flow, horizontal.'),
    ('Perp2', 'vec', (0, 0, 1), 'Across the flow, completing the frame.'),
    ('Rel', 'vec', (1000, 0, 0), 'The body part velocity through the air (cm/s): strokes run back along its path.'),
    ('Bend', 'vec', (0, 0, 0), 'The body part acceleration (cm/s2): bends the path like the body path.'),
    ('StrokeTime', 'float', 0.11, 'Seconds of path a stroke spans (length = air speed x this).'),
    ('Life', 'float', 0.45, 'Seconds from the birth of a stroke to its tail reaching its head.'),
    ('Drag', 'vec', (0, 0, 0), 'Velocity (cm/s) the wake carries new strokes with, fading over WakeTime.'),
    ('Drift', 'vec', (0, 0, 0), 'Wind plus downwash (cm/s) carrying the marked air.'),
    ('WakeTime', 'float', 0.6, 'Seconds over which dragged air slows down.'),
    ('Turbulence', 'float', 3, 'Ripple amplitude (cm) of the strokes.'),
    ('Visibility', 'float', 1, 'Opacity factor of new strokes (grows with air speed).'),
    ('Seed', 'float', 0, 'Per body part random seed.'),
    ('Color', 'color', (1, 1, 1, 0.85), 'Stroke color, alpha = opacity.'),
    ('Width', 'float', 3.5, 'Stroke width (cm) at its thickest.'),
    ('MinPixels', 'float', 1.6, 'Strokes are at least this wide on screen; thinner ones fade instead.'),
    ('PixelAngle', 'float', 0.001, 'Size of a pixel at 1 cm from the camera (set each frame).'),
    ('CameraPosition', 'pos', (0, 0, 0), 'Camera location (set each frame).'),
    ('NearFade', 'float', 300, 'Strokes fade out closer than this to the camera (cm).'),
    ('Stream', 'float', 0.35, 'Speed of the head of a stroke back along the path, as a fraction of the air speed.'),
    ('LoopChance', 'float', 0.25, 'Fraction of strokes doing a loop.'),
    ('LoopSize', 'float', 0.1, 'Loop radius as a fraction of the stroke length.'),
    ('FadeIn', 'float', 0.1, 'Seconds a new stroke takes to fade in (while the body leaves it behind).'),
]

KF = 'float(max(User.Points, 2))'
STREAK = '(floor((float(Particles.UniqueID) + 0.5) / ' + KF + '))'
TAU = '6.2831853'


def H(k):
    return ('frac(sin(fmod(' + STREAK + ', 7919.0) * ' + str(12.9898 + 1.37 * k) + ' + User.Seed * ' + str(78.233 + 3.1 * k)
            + ' + ' + str(4.1 * k) + ') * 43758.5453)')


# A stroke is the trail of its head: the head runs along a path through the air, every ribbon point (U = 0 head .. 1
# tail) lies on that same path at path time Tau, so the tail follows the head through its bends and loops. The path is
# closed form: back along the body part's path through the air (Stream x Rel, bent by Bend: shaped like the air
# streaming past), rippling, with now and then one loop; the downwash and the wind (Drift) carry the whole pattern, and
# at first the wake too (the body drags the air near it along: Drag fading over WakeTime). A loop is a smooth prolate cycloid
# (the head swings back faster than it runs) in a plane facing the camera. The head runs until TEnd, the tail Lag behind it: the stroke grows from the body, slides, then shrinks.
SPAWN = [
    [
        ('Particles.Streak', 'int', 'int(' + STREAK + ')'),
        ('Particles.U', 'float', 'fmod(float(Particles.UniqueID), ' + KF + ') / (' + KF + ' - 1.0)'),
    ] + [('Particles.H%d' % k, 'float', H(k)) for k in range(1, 9)],
    [
        # The component varies each stroke's life (it counts the strokes on screen).
        ('Particles.Lifetime', 'float', 'User.Life'),
        ('Particles.Base', 'pos', 'Particles.Position + User.Extent * (2.0 * Particles.H2 - 1.0) + User.Spread * sqrt(Particles.H3) * '
         '(cos(' + TAU + ' * Particles.H4) * User.Perp1 + sin(' + TAU + ' * Particles.H4) * User.Perp2)'),
        ('Particles.Rel', 'vec', 'User.Rel'),
        ('Particles.Bend', 'vec', 'User.Bend'),
        ('Particles.Drag', 'vec', 'User.Drag'),
        ('Particles.Drift', 'vec', 'User.Drift'),
        ('Particles.WakeTime', 'float', 'max(User.WakeTime, 0.01)'),
        ('Particles.Stream', 'float', 'max(User.Stream, 0.05)'),
        ('Particles.Ripple', 'vec', 'User.Turbulence * (0.5 + Particles.H3) * (cos(' + TAU + ' * Particles.H2) * User.Perp1 + sin(' + TAU + ' * Particles.H2) * User.Perp2)'),
        ('Particles.Phase', 'float', TAU + ' * Particles.H4'),
        ('Particles.Visibility', 'float', 'User.Visibility'),
        ('Particles.WidthJitter', 'float', '0.7 + 0.6 * Particles.H3'),
        ('Particles.RibbonLinkOrder', 'float', 'Particles.U'),
        ('Particles.LoopOn', 'float', 'saturate((User.LoopChance - Particles.H6) * 1000000.0)'),
        ('Particles.FadeIn', 'float', 'max(User.FadeIn, 0.001)'),
    ],
    [
        ('Particles.Lag', 'float', 'min(User.StrokeTime * (0.6 + 0.8 * Particles.H1) / Particles.Stream, 0.5 * Particles.Lifetime)'),
        ('Particles.HeadVelocity', 'vec', '-Particles.Stream * Particles.Rel'),
    ],
    [
        ('Particles.TEnd', 'float', 'Particles.Lifetime - Particles.Lag'),
        ('Particles.RippleFreq', 'float', TAU + ' * (0.8 + 0.6 * Particles.H5) / Particles.Lag'),
        ('Particles.LoopRadius', 'float', 'User.LoopSize * (0.7 + 0.6 * Particles.H7) * length(Particles.HeadVelocity) * Particles.Lag'),
        # Peak backward swing 2.5x the head's speed: duration = 4 pi R / (2.5 v) (the speed cancels out).
        ('Particles.LoopDuration', 'float', '5.0265 * User.LoopSize * (0.7 + 0.6 * Particles.H7) * Particles.Lag'),
        ('Particles.HeadDir', 'vec', 'Particles.HeadVelocity / max(length(Particles.HeadVelocity), 0.001)'),
    ],
    [
        ('Particles.LoopStart', 'float', '0.1 * Particles.TEnd + Particles.H8 * max(0.75 * Particles.TEnd - Particles.LoopDuration, 0.0)'),
        # No loop seen edge on, where the stroke runs towards or away from the camera.
        ('Particles.LoopOn', 'float', 'Particles.LoopOn * saturate((0.85 * Particles.TEnd - Particles.LoopDuration) * 1000000.0) '
         '* saturate(4.0 * (length(cross((Particles.Base - User.CameraPosition) / max(length(Particles.Base - User.CameraPosition), 1.0), Particles.HeadDir)) - 0.4))'),
        # Across the head's way and the view: the loop's plane faces the camera.
        ('Particles.LoopAcross', 'vec', 'cross(Particles.Base - User.CameraPosition, Particles.HeadDir)'),
    ],
    [
        ('Particles.LoopAcross', 'vec', '(floor(frac(Particles.H5 * 7.31) * 2.0) * 2.0 - 1.0) * lerp(User.Perp1, Particles.LoopAcross / max(length(Particles.LoopAcross), 0.0001), '
         'saturate(length(Particles.LoopAcross) / max(0.3 * length(Particles.Base - User.CameraPosition), 0.001)))'),
        ('Particles.LoopRadius', 'float', 'Particles.LoopRadius * Particles.LoopOn'),
    ],
    [
        # Tilted a little off the view, each loop its own way.
        ('Particles.LoopN', 'vec', 'cos((Particles.H7 - 0.5) * 1.2) * Particles.LoopAcross + sin((Particles.H7 - 0.5) * 1.2) * cross(Particles.HeadDir, Particles.LoopAcross)'),
    ],
    [
        # Square to the head's way, or the loop is sheared.
        ('Particles.LoopN', 'vec', '(Particles.LoopN - dot(Particles.LoopN, Particles.HeadDir) * Particles.HeadDir) '
         '/ max(length(Particles.LoopN - dot(Particles.LoopN, Particles.HeadDir) * Particles.HeadDir), 0.0001)'),
    ],
]
UPDATE = [
    [
        ('Particles.TauHead', 'float', 'min(Particles.Age, Particles.TEnd)'),
        ('Particles.TauTail', 'float', 'max(Particles.Age - Particles.Lag, 0.0)'),
    ],
    [
        ('Particles.Tau', 'float', 'lerp(Particles.TauHead, Particles.TauTail, Particles.U)'),
        ('Particles.Live', 'float', 'saturate(3.0 * (Particles.TauHead - Particles.TauTail) / max(Particles.Lag, 0.001))'),
    ],
    [
        ('Particles.LoopX', 'float', 'saturate((Particles.Tau - Particles.LoopStart) / max(Particles.LoopDuration, 0.001))'),
        # Thickest just behind the round head, tapering to the tail.
        ('Particles.Profile', 'float', 'pow(max(sin(3.14159265 * (0.12 + 0.88 * pow(max(Particles.U, 0.0001), 0.75))), 0.0001), 0.7) * Particles.Live'),
    ],
    [
        ('Particles.LoopPhi', 'float', TAU + ' * Particles.LoopX - sin(' + TAU + ' * Particles.LoopX)'),
        ('Particles.LineWidth', 'float', 'User.Width * Particles.WidthJitter * Particles.Profile'),
    ],
    [
        ('Particles.Position', 'pos', 'Particles.Base + Particles.Drift * Particles.Age + Particles.HeadVelocity * Particles.Tau '
         '+ Particles.Drag * Particles.WakeTime * (1.0 - exp(-Particles.Age / Particles.WakeTime)) '
         '+ 0.5 * Particles.Stream * Particles.Stream * Particles.Bend * Particles.Tau * Particles.Tau '
         '+ Particles.Ripple * (0.3 + Particles.Tau / max(Particles.TEnd, 0.01)) * sin(Particles.Phase + Particles.RippleFreq * Particles.Tau) '
         '+ Particles.LoopRadius * (sin(Particles.LoopPhi) * Particles.HeadDir + (1.0 - cos(Particles.LoopPhi)) * Particles.LoopN)'),
    ],
    [
        ('Particles.PixelWidth', 'float', 'User.MinPixels * User.PixelAngle * length(Particles.Position - User.CameraPosition)'),
    ],
    [
        ('Particles.RibbonWidth', 'float', 'max(Particles.LineWidth, Particles.PixelWidth)'),
        ('Particles.Color', 'color', 'float4(User.Color.rgb, User.Color.a * Particles.Visibility * saturate(Particles.LineWidth / max(Particles.PixelWidth, 0.001)) '
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
    out['saved'] = execute_tool('editor_toolset.toolsets.asset.AssetTools.save_assets', json.dumps({'asset_paths': ['/LBA2Importer/Effects/NS_LBA_Airflow']}))['returnValue']
    return out
