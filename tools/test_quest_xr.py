"""Run production XR input/presentation code against a small fake runtime.

No device, retail assets, Android runtime, or GPU required. This checks API
sequencing and geometry; it does not substitute for an on-headset test.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(text, name):
    match = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^;]*?\)\s*\{", text, re.M)
    assert match, name
    start = match.start()
    cursor = text.index("{", match.start())
    depth = 1
    end = cursor + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class QuestXRTest(unittest.TestCase):
    def test_production_input_and_layers(self):
        xr = (ROOT / "port/android/host/host_xr.c").read_text()
        pad = (ROOT / "port/linux/src/xinput_sdl.c").read_text()
        cinematics = (ROOT / "source/rasterizer/rasterizer_cinematics.c").read_text()
        main_source = (ROOT / "source/main/main.c").read_text()
        xbox = (ROOT / "port/include/xdk/xdk_xbox.h").read_text()
        bindings = xr[xr.index("static const struct touch_binding"):xr.index("static int create_actions")]
        structs = xr[xr.index("struct xr_eye"):xr.index("static char xr_last_error")]
        bodies = "\n".join(function(xr, n) for n in (
            "smooth_controller_pose", "hand_distance_squared_locked",
            "weapon_switch_allowed_locked",
            "update_two_hand_locked", "two_hand_active_locked", "apply_stick_deadzone",
            "host_xr_gamepad", "host_xr_two_hand_active", "host_xr_set_two_hand_allowed",
            "host_xr_dominant_hand", "host_xr_get_controller_pose",
            "host_xr_get_controller_velocity", "host_xr_haptic", "create_actions",
            "sync_actions", "locate_controller_poses",
            "host_xr_get_view", "host_xr_set_source_dimensions",
            "host_xr_end_frame"))
        bodies += "\n" + function(cinematics, "quest_xr_disable_convolution")
        bodies += "\n" + "\n".join(function(main_source, n) for n in (
            "quest_xr_rotate_vector", "quest_xr_map_vector",
            "quest_xr_projection_from_fov",
            "quest_xr_get_local_controller_pose",
            "quest_xr_get_head_look",
            "quest_xr_get_controller_transform", "quest_xr_get_controller_world_pose"))
        pointers = "\n".join("static PFN_" + n + " p_" + n + ";"
                             for n in sorted(set(re.findall(r"p_(xr\w+)", bodies))))
        defines = "\n".join(line for line in xbox.splitlines() if line.startswith("#define XINPUT_GAMEPAD_"))
        pose_state = """static struct xr_controller_pose controller_poses[POSE_ACTION_COUNT];
static XrTime controller_filter_time;
static float head_position[3];
static float head_orientation[4];
static int head_position_valid;
static int dominant_hand = 1;
static int source_width, source_height;
static int two_hand_held;
static int two_hand_allowed = 1;
"""
        code = "\n".join((PRELUDE, defines, structs, pose_state, pointers, bindings, bodies))
        code += function(pad, "merge_button") + function(pad, "touch_gamepad_state") + CHECKS
        with tempfile.TemporaryDirectory(prefix="halo-xr-test-") as directory:
            src = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            src.write_text(code)
            subprocess.run(["gcc", "-std=c11", "-Werror=implicit-function-declaration",
                            "-I", str(ROOT / "port/third_party/openxr/include"),
                            str(src), "-lm", "-lpthread", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
typedef unsigned GLuint, GLenum, GLbitfield, EGLenum;
typedef int GLint;
typedef unsigned char GLboolean;
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#define EYE_COUNT 2
#define TOUCH_ACTION_COUNT 13
#define POSE_ACTION_COUNT 2
#define HAPTIC_ACTION_COUNT 2
#define XR_ACTION_COUNT (TOUCH_ACTION_COUNT + POSE_ACTION_COUNT + HAPTIC_ACTION_COUNT)
#define GL_READ_FRAMEBUFFER_BINDING 1
#define GL_DRAW_FRAMEBUFFER_BINDING 2
#define GL_READ_FRAMEBUFFER 3
#define GL_DRAW_FRAMEBUFFER 4
#define GL_SCISSOR_TEST 5
#define GL_COLOR_ATTACHMENT0 6
#define GL_TEXTURE_2D 7
#define GL_COLOR_BUFFER_BIT 8
#define GL_LINEAR 9
#define GL_FRAMEBUFFER_COMPLETE 10
static float touch_state[15];
static pthread_mutex_t touch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t quest_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static float quest_snap_pending_yaw;
static int config_boolean(const char *name) { return !strcmp(name,"vr.snap_turn"); }
static double config_real(const char *name) { return !strcmp(name,"vr.snap_turn_degrees") ? 45.0 : 0.0; }
static int scissor = 1, blits, submitted, releases, acquire_count;
static int read_fbo = 11, draw_fbo = 12;
static int crop[2][4], crop_fbo[2];
static void glGetIntegerv(GLenum k, GLint *v) { *v = k == 1 ? read_fbo : draw_fbo; }
static GLboolean glIsEnabled(GLenum k) { return scissor; }
static void glDisable(GLenum k) { scissor = 0; }
static void glEnable(GLenum k) { scissor = 1; }
static void glBindFramebuffer(GLenum k, GLuint v) { if(k == 3) read_fbo=v; else draw_fbo=v; }
static void glFramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e) {}
static GLenum glCheckFramebufferStatus(GLenum k) { return GL_FRAMEBUFFER_COMPLETE; }
static void glBlitFramebuffer(int x0,int y0,int x1,int y1,int a,int b,int c,int d,GLbitfield m,GLenum f) {
    assert(!scissor); assert(read_fbo == 0 || read_fbo == 11); assert(blits < 2);
    crop_fbo[blits]=read_fbo;
    crop[blits][0]=x0; crop[blits][1]=y0; crop[blits][2]=x1; crop[blits][3]=y1; blits++;
}
static void glFlush(void) {}
static void host_fatal(const char *format, ...) { abort(); }
static const char *host_xr_last_error(void) { return "test"; }
static int xr_ok(XrResult r, const char *op) { return XR_SUCCEEDED(r); }
static int host_xr_begin_frame(void) { return 1; }
typedef int16_t SHORT;
typedef uint8_t BYTE;
typedef int BOOL;
typedef unsigned char boolean;
typedef float real;
typedef struct { real x0,x1,y0,y1; } real_rectangle2d;
typedef union real_vector3d {
    struct { real i,j,k; };
    struct { real x,y,z; };
} real_vector3d;
typedef union real_point3d {
    struct { real x,y,z; };
    struct { real i,j,k; };
} real_point3d;
static real_vector3d test_up={.i=0,.j=0,.k=1};
static real_vector3d *global_up3d=&test_up;
static real normalize3d(real_vector3d *v) {
    real length=sqrtf(v->i*v->i+v->j*v->j+v->k*v->k);
    if(length>1e-6f) { v->i/=length; v->j/=length; v->k/=length; return length; }
    return 0;
}
static real_vector3d *cross_product3d(real_vector3d const *a,real_vector3d const *b,real_vector3d *r) {
    *r=(real_vector3d){.i=a->j*b->k-a->k*b->j,.j=a->k*b->i-a->i*b->k,.k=a->i*b->j-a->j*b->i}; return r;
}
static real dot_product3d(real_vector3d const *a,real_vector3d const *b) {
    return a->i*b->i+a->j*b->j+a->k*b->k;
}
#define TRUE 1
#define FALSE 0
#define DEGREES_TO_RADIANS(v) ((v)*(real)(M_PI/180.0))
#define tangent tanf
#define arctangent atan2f
struct rasterizer_cinematic_screen_effect_parameters {
    short convolution_extra_passes, convolution_type; float convolution_radius;
};
typedef struct { uint16_t wButtons; BYTE bAnalogButtons[8]; SHORT sThumbLX,sThumbLY,sThumbRX,sThumbRY; } XINPUT_GAMEPAD;
'''

CHECKS = r'''
static unsigned action_count, path_count, space_count, haptic_count;
static int active_input = 1;
static int pose_valid = 1;
static float test_velocity_y[2];
static XrResult make_set(XrInstance i, const XrActionSetCreateInfo *c, XrActionSet *s) {
    assert(!strcmp(c->actionSetName,"halo")); *s=(XrActionSet)(uintptr_t)100; return XR_SUCCESS;
}
static XrResult make_action(XrActionSet s, const XrActionCreateInfo *c, XrAction *a) {
    assert(c->actionType == touch_bindings[action_count].type);
    *a=(XrAction)(uintptr_t)(++action_count); return XR_SUCCESS;
}
static XrResult path(XrInstance i,const char *s,XrPath *p) { assert(s[0]=='/'); *p=++path_count; return XR_SUCCESS; }
static XrResult suggest(XrInstance i,const XrInteractionProfileSuggestedBinding *s) {
    assert(s->countSuggestedBindings==17); assert(s->interactionProfile==18);
    for(unsigned j=0;j<17;j++) assert(s->suggestedBindings[j].binding==j+1);
    return XR_SUCCESS;
}
static XrResult attach(XrSession s,const XrSessionActionSetsAttachInfo *a) {
    assert(a->countActionSets==1); assert(a->actionSets[0]==xr.action_set); return XR_SUCCESS;
}
static XrResult make_action_space(XrSession s,const XrActionSpaceCreateInfo *i,XrSpace *space) {
    assert(i->action==(XrAction)(uintptr_t)(14+space_count));
    assert(i->subactionPath==XR_NULL_PATH && i->poseInActionSpace.orientation.w==1.0f);
    *space=(XrSpace)(uintptr_t)(200+space_count++); return XR_SUCCESS;
}
static XrResult locate_space(XrSpace space,XrSpace base,XrTime time,XrSpaceLocation *location) {
    assert(base==xr.space && time==xr.predicted_time);
    unsigned hand=(unsigned)((uintptr_t)space-200);
    assert(hand<2);
    location->locationFlags=pose_valid ?
        XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT : 0;
    location->pose.position=(XrVector3f){(float)hand,2.0f,3.0f};
    location->pose.orientation=(XrQuaternionf){0.0f,(float)hand,0.0f,1.0f};
    XrSpaceVelocity *velocity=(XrSpaceVelocity *)location->next;
    assert(velocity && velocity->type==XR_TYPE_SPACE_VELOCITY);
    velocity->velocityFlags=XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
    velocity->linearVelocity=(XrVector3f){0.0f,test_velocity_y[hand],0.0f};
    return XR_SUCCESS;
}
static XrResult apply_haptic(XrSession session,const XrHapticActionInfo *info,const XrHapticBaseHeader *base) {
    const XrHapticVibration *vibration=(const XrHapticVibration *)base;
    assert(info->action==(XrAction)(uintptr_t)(haptic_count ? 16 : 17));
    assert(vibration->type==XR_TYPE_HAPTIC_VIBRATION);
    assert(vibration->amplitude==1.0f && vibration->duration==400000000 && vibration->frequency==100.0f);
    haptic_count++; return XR_SUCCESS;
}
static XrResult sync_input(XrSession s,const XrActionsSyncInfo *a) { assert(a->countActiveActionSets==1); return XR_SUCCESS; }
static XrResult boolean_input(XrSession s,const XrActionStateGetInfo *g,XrActionStateBoolean *v) {
    v->isActive=active_input; v->currentState=1; return XR_SUCCESS;
}
static XrResult float_input(XrSession s,const XrActionStateGetInfo *g,XrActionStateFloat *v) {
    v->isActive=active_input; v->currentState=1; return XR_SUCCESS;
}
static XrResult vector_input(XrSession s,const XrActionStateGetInfo *g,XrActionStateVector2f *v) {
    v->isActive=active_input; v->currentState=(XrVector2f){-1,1}; return XR_SUCCESS;
}
static XrResult acquire(XrSwapchain s,const XrSwapchainImageAcquireInfo *i,uint32_t *index) {
    assert(s==xr.eyes[acquire_count].swapchain); acquire_count++; *index=0; return XR_SUCCESS;
}
static XrResult wait_image(XrSwapchain s,const XrSwapchainImageWaitInfo *i) { return XR_SUCCESS; }
static XrResult release(XrSwapchain s,const XrSwapchainImageReleaseInfo *i) { releases++; return XR_SUCCESS; }
static XrResult end_frame(XrSession s,const XrFrameEndInfo *e) {
    submitted++;
    assert(e->layerCount == (xr.should_render ? 1u : 0u));
    if(!e->layerCount) return XR_SUCCESS;
    if(!xr.stereo) {
        const XrCompositionLayerQuad *q=(const void *)e->layers[0];
        assert(q->type==XR_TYPE_COMPOSITION_LAYER_QUAD);
        assert(q->eyeVisibility==XR_EYE_VISIBILITY_BOTH);
        assert(q->space==xr.view_space && q->pose.position.z==-2);
        assert(q->pose.orientation.w==1 && q->size.width/q->size.height==2);
        assert(q->subImage.swapchain==xr.eyes[0].swapchain);
    } else {
        const XrCompositionLayerProjection *p=(const void *)e->layers[0];
        assert(p->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION && p->viewCount==2);
        for(int i=0;i<2;i++) {
            float pos[3], ori[4], fov[4];
            assert(host_xr_get_view(i,pos,ori,fov));
            assert(p->views[i].fov.angleLeft==fov[0] && p->views[i].fov.angleUp==fov[2]);
            assert(p->views[i].subImage.swapchain==xr.eyes[i].swapchain);
            assert(p->views[i].pose.position.x==pos[0]);
        }
    }
    return XR_SUCCESS;
}
int main(void) {
    float asymmetric_fov[4]={-0.9f,1.0f,0.95f,-0.8f};
    real vertical_fov;
    real_rectangle2d asymmetric_bounds;
    assert(quest_xr_projection_from_fov(asymmetric_fov,&vertical_fov,&asymmetric_bounds));
    assert(asymmetric_bounds.x0<0 && asymmetric_bounds.x1>0);
    assert(fabsf(asymmetric_bounds.x0+asymmetric_bounds.x1)>0.01f);
    assert(fabsf(asymmetric_bounds.y0+asymmetric_bounds.y1)>0.01f);
    float invalid_fov[4]={0.5f,-0.5f,0.5f,-0.5f};
    assert(!quest_xr_projection_from_fov(invalid_fov,&vertical_fov,&asymmetric_bounds));
    struct rasterizer_cinematic_screen_effect_parameters effect={3,2,4.0f};
    assert(!quest_xr_disable_convolution(&effect,1));
    assert(effect.convolution_extra_passes==3 && effect.convolution_type==2 && effect.convolution_radius==4.0f);
    assert(quest_xr_disable_convolution(&effect,2));
    assert(!effect.convolution_extra_passes && !effect.convolution_type && effect.convolution_radius==0.0f);
    p_xrCreateActionSet=make_set; p_xrCreateAction=make_action; p_xrStringToPath=path;
    p_xrSuggestInteractionProfileBindings=suggest; p_xrAttachSessionActionSets=attach;
    p_xrCreateActionSpace=make_action_space; p_xrLocateSpace=locate_space;
    p_xrApplyHapticFeedback=apply_haptic;
    p_xrSyncActions=sync_input; p_xrGetActionStateBoolean=boolean_input;
    p_xrGetActionStateFloat=float_input; p_xrGetActionStateVector2f=vector_input;
    assert(create_actions()); assert(action_count==17 && space_count==2);
    xr.session_running=1; xr.session_state=XR_SESSION_STATE_FOCUSED;
    host_xr_haptic(1,1.0f,0.4f,100.0f); assert(haptic_count==1);
    sync_actions(); XINPUT_GAMEPAD pad={0}; touch_gamepad_state(&pad);
    assert(pad.sThumbLX==-32767 && pad.sThumbLY==32767);
    assert(pad.sThumbRX==0 && pad.sThumbRY==0);
    assert(fabsf(quest_snap_pending_yaw-(float)(M_PI/4.0))<1e-6f);
    for(int i=0;i<4;i++) assert(pad.bAnalogButtons[i]==255);
    assert(pad.bAnalogButtons[XINPUT_GAMEPAD_BLACK]==255 && !pad.bAnalogButtons[XINPUT_GAMEPAD_WHITE]);
    assert(pad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]==255);
    assert(pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]==255);
    assert(pad.wButtons==(XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB));
    active_input=0; sync_actions(); memset(&pad,0,sizeof(pad)); touch_gamepad_state(&pad);
    assert(!pad.wButtons && !pad.sThumbLX && !pad.bAnalogButtons[0]);
    active_input=1; sync_actions(); xr.session_state=XR_SESSION_STATE_VISIBLE; sync_actions();
    for(int i=0;i<15;i++) assert(touch_state[i]==0);
    xr.space=(XrSpace)(uintptr_t)2; xr.predicted_time=1234;
    xr.session_state=XR_SESSION_STATE_FOCUSED; locate_controller_poses();
    float hand_position[3], hand_orientation[4];
    assert(host_xr_get_controller_pose(1,hand_position,hand_orientation));
    assert(hand_position[0]==1.0f && hand_position[1]==2.0f && hand_orientation[1]==1.0f);
    float hand_velocity[3];
    assert(host_xr_get_controller_velocity(1,hand_velocity));
    assert(hand_velocity[0]==0.0f && hand_velocity[1]==0.0f && hand_velocity[2]==0.0f);
    pose_valid=0; locate_controller_poses();
    assert(!host_xr_get_controller_pose(1,hand_position,hand_orientation));
    pose_valid=1; xr.session_state=XR_SESSION_STATE_VISIBLE; locate_controller_poses();
    assert(!host_xr_get_controller_pose(0,hand_position,hand_orientation));
    xr.session_state=XR_SESSION_STATE_FOCUSED; xr.session_running=0; locate_controller_poses();
    assert(!host_xr_get_controller_pose(1,hand_position,hand_orientation));
    xr.session_running=1;
    xr.view_count=2; xr.recentered=1;
    xr.views[0].pose.position=(XrVector3f){0.1f,0.2f,0.3f};
    xr.views[0].pose.orientation.w=1.0f;
    controller_poses[1].valid=1;
    controller_poses[1].position[0]=0.4f;
    controller_poses[1].position[1]=0.5f;
    controller_poses[1].position[2]=0.6f;
    controller_poses[1].orientation[0]=0;
    controller_poses[1].orientation[1]=0;
    controller_poses[1].orientation[2]=0;
    controller_poses[1].orientation[3]=1;
    real_point3d eye_world={.x=10,.y=20,.z=30}, tracked;
    real_vector3d body={.i=1,.j=0,.k=0}, tracked_forward, tracked_up;
    assert(quest_xr_get_head_look(&body,&tracked_forward));
    assert(fabsf(tracked_forward.i-1.0f)<1e-5f && fabsf(tracked_forward.k)<1e-5f);
    xr.views[0].pose.orientation.x=sinf((float)M_PI/12.0f);
    xr.views[0].pose.orientation.w=cosf((float)M_PI/12.0f);
    assert(quest_xr_get_head_look(&body,&tracked_forward));
    assert(tracked_forward.k>0.49f);
    xr.views[0].pose.orientation.x=0.0f; xr.views[0].pose.orientation.w=1.0f;
    assert(quest_xr_get_controller_transform(&eye_world,&body,0,&tracked,&tracked_forward,&tracked_up));
    assert(fabsf(tracked.x-9.9015748f)<1e-5f && fabsf(tracked.y-19.9015748f)<1e-5f);
    assert(fabsf(tracked.z-30.0984252f)<1e-5f);
    assert(tracked_forward.i==1 && tracked_forward.j==0 && tracked_forward.k==0);
    assert(tracked_up.i==0 && tracked_up.j==0 && tracked_up.k==1);
    assert(quest_xr_get_controller_world_pose(&eye_world,&body,1,&tracked,&tracked_forward,&tracked_up));
    assert(fabsf(tracked.x-9.8031496f)<1e-5f && fabsf(tracked.y-19.8687664f)<1e-5f);
    assert(fabsf(tracked.z-30.1640420f)<1e-5f);
    controller_poses[0].valid=1;
    controller_poses[0].position[0]=0.4f;
    controller_poses[0].position[1]=0.5f;
    controller_poses[0].position[2]=0.25f;
    controller_poses[0].orientation[3]=1;
    touch_state[13]=1;
    float suppressed_pad[15]; host_xr_gamepad(suppressed_pad);
	assert(host_xr_two_hand_active());
	host_xr_set_two_hand_allowed(0);
	assert(!host_xr_two_hand_active());
	host_xr_set_two_hand_allowed(1);
    assert(suppressed_pad[13]==0);
    assert(quest_xr_get_controller_world_pose(&eye_world,&body,1,&tracked,&tracked_forward,&tracked_up));
    assert(fabsf(tracked_forward.i-1.0f)<1e-5f && fabsf(tracked_forward.j)<1e-5f);
    host_xr_haptic(0,1.0f,0.4f,100.0f); assert(haptic_count==2);

    /* Off-hand grip explicitly engages support-hand aiming; releasing it with
       crossed controllers drops back to one hand. The grip remains consumed. */
    memset(touch_state,0,sizeof(touch_state)); dominant_hand=1;
    host_xr_gamepad(suppressed_pad);
    controller_poses[0].position[0]=0.30f; controller_poses[0].position[1]=0;
    controller_poses[0].position[2]=0;
    controller_poses[1].position[0]=0.40f; controller_poses[1].position[1]=0;
    controller_poses[1].position[2]=0;
    touch_state[13]=1.0f;
    float mapped[15]; host_xr_gamepad(mapped);
    assert(host_xr_dominant_hand()==1 && host_xr_two_hand_active());
    assert(mapped[13]==0.0f && mapped[14]==0.0f);
    touch_state[13]=0.0f; host_xr_gamepad(mapped);
    assert(!host_xr_two_hand_active());

    /* VR gestures feed the corresponding Xbox actions. */
    controller_poses[0].velocity_valid=1;
    controller_poses[0].linear_velocity[1]=3.0f;
    controller_poses[0].position[1]=-0.2f;
    head_position_valid=1; head_position[0]=controller_poses[0].position[0];
    head_position[1]=-0.2f; head_position[2]=controller_poses[0].position[2];
    host_xr_gamepad(mapped);
    assert(mapped[7]==1.0f && mapped[13]==1.0f && mapped[11]==1.0f);

    /* Y is the ordinary switch-weapon action; dominant grip changes grenade type. */
    controller_poses[0].linear_velocity[1]=0; touch_state[7]=1.0f;
    head_orientation[0]=head_orientation[1]=head_orientation[2]=0;
    head_orientation[3]=1;
    controller_poses[1].position[0]=1.0f; touch_state[9]=1.0f;
    host_xr_gamepad(mapped); assert(mapped[9]==1.0f);
    touch_state[9]=0.0f; touch_state[14]=1.0f;
    host_xr_gamepad(mapped); assert(mapped[14]==1.0f);

    head_position_valid=0; touch_state[13]=0; touch_state[14]=0;
    host_xr_gamepad(mapped);
    controller_poses[0].position[0]=0.3f; controller_poses[0].position[1]=0.0f;
    controller_poses[0].position[2]=-0.35f;
    controller_poses[1].position[0]=0.3f; controller_poses[1].position[1]=0.0f;
    controller_poses[1].position[2]=0.0f;
    host_xr_gamepad(mapped);
	/* Proximity alone must not silently seize the firing direction. */
	assert(!host_xr_two_hand_active());
    assert(quest_xr_get_controller_world_pose(&eye_world,&body,-1,&tracked,&tracked_forward,&tracked_up));
    p_xrAcquireSwapchainImage=acquire; p_xrWaitSwapchainImage=wait_image;
    p_xrReleaseSwapchainImage=release; p_xrEndFrame=end_frame;
    xr.session=(XrSession)(uintptr_t)1; xr.space=(XrSpace)(uintptr_t)2;
    xr.view_space=(XrSpace)(uintptr_t)3;
    xr.frame_begun=1; xr.should_render=1; xr.view_count=2; xr.recentered=1;
    XrSwapchainImageOpenGLESKHR images[2]={{0},{0}};
    for(int i=0;i<2;i++) {
        xr.eyes[i].swapchain=(XrSwapchain)(uintptr_t)(10+i);
        xr.eyes[i].images=&images[i]; xr.eyes[i].width=1000; xr.eyes[i].height=1000;
        xr.views[i].fov=(XrFovf){-0.9f,1.0f,0.95f,-0.8f};
        xr.views[i].pose.position.x=i ? .032f : -.032f;
    }
    assert(host_xr_end_frame(1200,800)); assert(blits==1 && releases==1);
    assert(crop[0][0]==0 && crop[0][1]==100 && crop[0][2]==1200 && crop[0][3]==700);
    assert(scissor && read_fbo==11 && draw_fbo==12);
    xr.stereo=1; xr.frame_begun=1; blits=releases=acquire_count=0;
    host_xr_set_source_dimensions(1920,960);
    assert(host_xr_end_frame(1200,800)); assert(blits==2 && releases==2);
    assert(crop_fbo[0]==11 && crop_fbo[1]==11);
    assert(crop[0][0]==0 && crop[0][1]==960 && crop[0][2]==960 && crop[0][3]==0);
    assert(crop[1][0]==960 && crop[1][1]==960 && crop[1][2]==1920 && crop[1][3]==0);
    xr.should_render=0; xr.frame_begun=1; blits=releases=acquire_count=0;
    assert(host_xr_end_frame(1200,800)); assert(blits==0 && releases==0 && submitted==3);
    puts("PASS: Touch attach/sync/poses/haptics, one/two-hand transforms, grip suppression, Xbox input, focus release, menu quad, stereo FOV/crops, GL state, no-render frame");
    return 0;
}
'''

if __name__ == "__main__":
    unittest.main()
