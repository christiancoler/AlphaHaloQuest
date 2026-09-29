/*
HOST_XR.C

Minimal OpenXR/OpenGL ES presentation for standalone Quest. The game renders
two square views side-by-side in its ordinary back buffer and the halves are
copied into runtime-owned eye swapchains. The loader is opened dynamically so
the normal Android error UI still works outside a headset.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#define XR_NO_PROTOTYPES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define EYE_COUNT 2
#define TOUCH_ACTION_COUNT 13
#define POSE_ACTION_COUNT 2
#define HAPTIC_ACTION_COUNT 2
#define XR_ACTION_COUNT (TOUCH_ACTION_COUNT + POSE_ACTION_COUNT + HAPTIC_ACTION_COUNT)

struct xr_eye
{
	XrSwapchain swapchain;
	uint32_t width, height, image_count;
	XrSwapchainImageOpenGLESKHR *images;
};

struct xr_controller_pose
{
	float position[3];
	float orientation[4];
	float linear_velocity[3];
	int valid;
	int velocity_valid;
};

struct xr_state
{
	void *loader;
	XrInstance instance;
	XrSystemId system;
	XrSession session;
	XrSpace space;
	XrSpace view_space;
	XrSessionState session_state;
	int initialized, session_running, frame_begun, should_render, stereo;
	XrTime predicted_time;
	XrView views[EYE_COUNT];
	uint32_t view_count;
	struct xr_eye eyes[EYE_COUNT];
	GLuint framebuffer;
	int recentered;
	int ever_running;
	XrActionSet action_set;
	XrAction actions[XR_ACTION_COUNT];
	XrSpace controller_spaces[POSE_ACTION_COUNT];
	XrFoveationProfileFB foveation_profile;
	uint64_t session_created_ticks;
};

static struct xr_state xr;
static char xr_last_error[256] = "OpenXR initialization did not complete.";

static void xr_errorf(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(xr_last_error, sizeof(xr_last_error), format, arguments);
	va_end(arguments);
}

const char *host_xr_last_error(void)
{
	return xr_last_error;
}

static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;
static PFN_xrInitializeLoaderKHR p_xrInitializeLoaderKHR;
static PFN_xrCreateInstance p_xrCreateInstance;
static PFN_xrDestroyInstance p_xrDestroyInstance;
static PFN_xrGetSystem p_xrGetSystem;
static PFN_xrGetOpenGLESGraphicsRequirementsKHR p_xrGetOpenGLESGraphicsRequirementsKHR;
static PFN_xrCreateSession p_xrCreateSession;
static PFN_xrDestroySession p_xrDestroySession;
static PFN_xrCreateReferenceSpace p_xrCreateReferenceSpace;
static PFN_xrDestroySpace p_xrDestroySpace;
static PFN_xrEnumerateViewConfigurationViews p_xrEnumerateViewConfigurationViews;
static PFN_xrEnumerateSwapchainFormats p_xrEnumerateSwapchainFormats;
static PFN_xrCreateSwapchain p_xrCreateSwapchain;
static PFN_xrDestroySwapchain p_xrDestroySwapchain;
static PFN_xrEnumerateSwapchainImages p_xrEnumerateSwapchainImages;
static PFN_xrPollEvent p_xrPollEvent;
static PFN_xrBeginSession p_xrBeginSession;
static PFN_xrEndSession p_xrEndSession;
static PFN_xrWaitFrame p_xrWaitFrame;
static PFN_xrBeginFrame p_xrBeginFrame;
static PFN_xrEndFrame p_xrEndFrame;
static PFN_xrLocateViews p_xrLocateViews;
static PFN_xrAcquireSwapchainImage p_xrAcquireSwapchainImage;
static PFN_xrWaitSwapchainImage p_xrWaitSwapchainImage;
static PFN_xrReleaseSwapchainImage p_xrReleaseSwapchainImage;
static PFN_xrStringToPath p_xrStringToPath;
static PFN_xrCreateActionSet p_xrCreateActionSet;
static PFN_xrCreateAction p_xrCreateAction;
static PFN_xrSuggestInteractionProfileBindings p_xrSuggestInteractionProfileBindings;
static PFN_xrAttachSessionActionSets p_xrAttachSessionActionSets;
static PFN_xrSyncActions p_xrSyncActions;
static PFN_xrGetActionStateBoolean p_xrGetActionStateBoolean;
static PFN_xrGetActionStateFloat p_xrGetActionStateFloat;
static PFN_xrGetActionStateVector2f p_xrGetActionStateVector2f;
static PFN_xrCreateActionSpace p_xrCreateActionSpace;
static PFN_xrLocateSpace p_xrLocateSpace;
static PFN_xrApplyHapticFeedback p_xrApplyHapticFeedback;
static PFN_xrPerfSettingsSetPerformanceLevelEXT p_xrPerfSettingsSetPerformanceLevelEXT;
static PFN_xrCreateFoveationProfileFB p_xrCreateFoveationProfileFB;
static PFN_xrDestroyFoveationProfileFB p_xrDestroyFoveationProfileFB;
static PFN_xrUpdateSwapchainFB p_xrUpdateSwapchainFB;

/* Snapshot ABI: LX, LY, RX, RY, LT, RT, A, B, X, Y, menu, L-click,
R-click, L-grip, R-grip. Only fixed-width floats cross the guest ABI. */
static float touch_state[15];
static struct xr_controller_pose controller_poses[POSE_ACTION_COUNT];
static XrTime controller_filter_time;
static float head_position[3];
static float head_orientation[4];
static int head_position_valid;
static int dominant_hand = 1;
static int source_width, source_height;
static int two_hand_held;
static int two_hand_allowed = 1;
static pthread_mutex_t touch_lock = PTHREAD_MUTEX_INITIALIZER;

static void smooth_controller_pose(struct xr_controller_pose *filtered,
	struct xr_controller_pose const *sample, float dt)
{
	float dx, dy, dz, distance, position_tau, position_alpha;
	float dot, sign, rotation_tau, rotation_alpha, length;
	unsigned i;

	if (!sample->valid)
	{
		memset(filtered, 0, sizeof(*filtered));
		return;
	}
	if (!filtered->valid || dt <= 0.0f || dt > 0.1f)
	{
		*filtered = *sample;
		return;
	}

	dx = sample->position[0] - filtered->position[0];
	dy = sample->position[1] - filtered->position[1];
	dz = sample->position[2] - filtered->position[2];
	distance = sqrtf(dx*dx + dy*dy + dz*dz);
	/* Suppress millimetre-scale tracking shimmer while letting deliberate
	   weapon movements catch up quickly. */
	position_tau = distance > 0.10f ? 0.010f : (distance > 0.03f ? 0.025f : 0.055f);
	position_alpha = 1.0f - expf(-dt / position_tau);
	for (i = 0; i < 3; ++i)
		filtered->position[i] +=
			(sample->position[i] - filtered->position[i]) * position_alpha;

	/* Normalized linear interpolation is stable for the small frame-to-frame
	   rotations produced by Touch tracking. Select the shortest quaternion arc. */
	dot = 0.0f;
	for (i = 0; i < 4; ++i)
		dot += filtered->orientation[i] * sample->orientation[i];
	sign = dot < 0.0f ? -1.0f : 1.0f;
	if (dot < 0.0f)
		dot = -dot;
	rotation_tau = dot < 0.97f ? 0.010f : 0.045f;
	rotation_alpha = 1.0f - expf(-dt / rotation_tau);
	length = 0.0f;
	for (i = 0; i < 4; ++i)
	{
		filtered->orientation[i] +=
			(sign * sample->orientation[i] - filtered->orientation[i]) * rotation_alpha;
		length += filtered->orientation[i] * filtered->orientation[i];
	}
	length = sqrtf(length);
	if (length > 0.00001f)
		for (i = 0; i < 4; ++i)
			filtered->orientation[i] /= length;
	else
		memcpy(filtered->orientation, sample->orientation,
			sizeof(filtered->orientation));

	/* Gesture detection needs the runtime's unsmoothed velocity. */
	memcpy(filtered->linear_velocity, sample->linear_velocity,
		sizeof(filtered->linear_velocity));
	filtered->velocity_valid = sample->velocity_valid;
	filtered->valid = 1;
}

static float hand_distance_squared_locked(void)
{
	float x, y, z;

	if (!two_hand_allowed || !controller_poses[0].valid || !controller_poses[1].valid)
		return INFINITY;
	x = controller_poses[0].position[0] - controller_poses[1].position[0];
	y = controller_poses[0].position[1] - controller_poses[1].position[1];
	z = controller_poses[0].position[2] - controller_poses[1].position[2];
	return x*x + y*y + z*z;
}

/* Retained as the compatibility hook used by the XR production test. Weapon
   switching is now always available on Y; the grip is reserved for grenade
   selection and is no longer gated by a shoulder-distance gesture. */
static int weapon_switch_allowed_locked(void)
{
	return 1;
}

static void update_two_hand_locked(void)
{
	float distance_squared = hand_distance_squared_locked();
	int offhand = 1 - dominant_hand;
	int support_grip;
	float to_support[3];

	if (!two_hand_allowed || !controller_poses[0].valid || !controller_poses[1].valid)
	{
		two_hand_held = 0;
		return;
	}
	to_support[0] = controller_poses[offhand].position[0] -
		controller_poses[dominant_hand].position[0];
	to_support[1] = controller_poses[offhand].position[1] -
		controller_poses[dominant_hand].position[1];
	to_support[2] = controller_poses[offhand].position[2] -
		controller_poses[dominant_hand].position[2];
	support_grip = touch_state[13 + offhand] > 0.55f;

	/* Long guns require the support-hand grip. An earlier proximity latch could
	   engage while the second controller was merely nearby and abruptly replace
	   the firing direction with the line between the hands. Pistol bracing can
	   still snap on at close range, but it never changes the firing direction. */
	if (two_hand_allowed == 2)
	{
		int close_brace = to_support[1] < 0.08f && to_support[1] > -0.20f &&
			distance_squared > 0.05f*0.05f &&
			distance_squared < (two_hand_held ? 0.26f*0.26f : 0.20f*0.20f);
		two_hand_held = support_grip || close_brace;
	}
	else
	{
		two_hand_held = support_grip &&
			distance_squared > 0.05f*0.05f && distance_squared < 0.62f*0.62f;
	}
}

static int two_hand_active_locked(void)
{
	/* Preserve the active weapon mode for the guest. Mode 1 may use the
	   support hand as a long-gun forward axis; mode 2 is a visual pistol brace. */
	return two_hand_held ? two_hand_allowed : 0;
}

static void apply_stick_deadzone(float *x, float *y)
{
	const float deadzone = 0.18f;
	float length = sqrtf(*x * *x + *y * *y);
	float scaled;

	if (length <= deadzone)
	{
		*x = 0.0f;
		*y = 0.0f;
		return;
	}
	if (length > 1.0f)
		length = 1.0f;
	scaled = (length - deadzone) / (1.0f - deadzone);
	*x = (*x / length) * scaled;
	*y = (*y / length) * scaled;
}

void host_xr_gamepad(float *state)
{
	float raw[15];
	int offhand;
	int motion_melee;
	int flashlight_gesture = 0;
	float dx, dy, dz;

	pthread_mutex_lock(&touch_lock);
	memcpy(raw, touch_state, sizeof(raw));
	apply_stick_deadzone(&raw[0], &raw[1]);
	apply_stick_deadzone(&raw[2], &raw[3]);
	/* Head tracking owns pitch in VR. Right-stick Y otherwise rotates the
	   flat-game camera under the headset and makes the whole world tilt. */
	raw[3] = 0.0f;
	if (head_position_valid)
	{
		float yaw = atan2f(2.0f * (head_orientation[3]*head_orientation[1] +
			head_orientation[0]*head_orientation[2]),
			1.0f - 2.0f * (head_orientation[0]*head_orientation[0] +
			head_orientation[1]*head_orientation[1]));
		float cosine = cosf(yaw);
		float sine = sinf(yaw);
		float x = raw[0];
		float y = raw[1];

		/* Snap turns already rotate Halo's facing. Apply only physical headset
		   yaw here so forward movement follows a real-world body turn. */
		raw[0] = x*cosine - y*sine;
		raw[1] = y*cosine + x*sine;
	}
	update_two_hand_locked();
	memcpy(state, raw, sizeof(raw));
	offhand = 1 - dominant_hand;
	/* Keep an ordinary, discoverable weapon-change button. The off-hand
	squeeze remains dedicated to the two-hand latch; the dominant squeeze is
	the otherwise unreachable Xbox black/switch-grenade input. */
	state[9] = raw[9];
	state[13] = 0.0f;
	state[14] = raw[14];

	/* A fast vertical hand swing triggers melee. */
	motion_melee =
		(controller_poses[0].velocity_valid && fabsf(controller_poses[0].linear_velocity[1]) > 2.5f) ||
		(controller_poses[1].velocity_valid && fabsf(controller_poses[1].linear_velocity[1]) > 2.5f);
	if (motion_melee)
		state[7] = 1.0f;

	/* Touching the headset with the off-hand toggles the flashlight. */
	if (head_position_valid && controller_poses[offhand].valid)
	{
		dx = controller_poses[offhand].position[0] - head_position[0];
		dy = controller_poses[offhand].position[1] - head_position[1];
		dz = controller_poses[offhand].position[2] - head_position[2];
		flashlight_gesture = dx*dx + dy*dy + dz*dz < 0.2f*0.2f;
	}
	if (flashlight_gesture)
		state[13] = 1.0f;
	if (head_position_valid && head_position[1] < -0.15f)
		state[11] = 1.0f;
	pthread_mutex_unlock(&touch_lock);
}

int host_xr_two_hand_active(void)
{
	int active;
	pthread_mutex_lock(&touch_lock);
	active = two_hand_active_locked();
	pthread_mutex_unlock(&touch_lock);
	return active;
}

void host_xr_set_two_hand_allowed(int allowed)
{
	pthread_mutex_lock(&touch_lock);
	two_hand_allowed = allowed;
	if (!two_hand_allowed)
		two_hand_held = 0;
	pthread_mutex_unlock(&touch_lock);
}

int host_xr_dominant_hand(void)
{
	int hand;
	pthread_mutex_lock(&touch_lock);
	hand = dominant_hand;
	pthread_mutex_unlock(&touch_lock);
	return hand;
}

int host_xr_get_controller_pose(int hand, float *position, float *orientation)
{
	int valid;

	if (hand < 0 || hand >= POSE_ACTION_COUNT || !position || !orientation)
		return 0;
	pthread_mutex_lock(&touch_lock);
	valid = controller_poses[hand].valid;
	if (valid)
	{
		memcpy(position, controller_poses[hand].position, sizeof(controller_poses[hand].position));
		memcpy(orientation, controller_poses[hand].orientation, sizeof(controller_poses[hand].orientation));
	}
	pthread_mutex_unlock(&touch_lock);
	return valid;
}

int host_xr_get_controller_velocity(int hand, float *linear_velocity)
{
	int valid;

	if (hand < 0 || hand >= POSE_ACTION_COUNT || !linear_velocity)
		return 0;
	pthread_mutex_lock(&touch_lock);
	valid = controller_poses[hand].velocity_valid;
	if (valid)
		memcpy(linear_velocity, controller_poses[hand].linear_velocity,
			sizeof(controller_poses[hand].linear_velocity));
	pthread_mutex_unlock(&touch_lock);
	return valid;
}

void host_xr_haptic(int hand, float amplitude, float duration_seconds, float frequency)
{
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };

	if (hand < 0 || hand >= HAPTIC_ACTION_COUNT ||
		!xr.session_running || xr.session_state != XR_SESSION_STATE_FOCUSED)
		return;
	info.action = xr.actions[TOUCH_ACTION_COUNT + POSE_ACTION_COUNT + hand];
	vibration.amplitude = fmaxf(0.0f, fminf(amplitude, 1.0f));
	vibration.duration = duration_seconds > 0.0f ?
		(XrDuration)(duration_seconds * 1000000000.0f) : XR_MIN_HAPTIC_DURATION;
	vibration.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;
	p_xrApplyHapticFeedback(xr.session, &info, (XrHapticBaseHeader *)&vibration);
}

static int xr_ok(XrResult result, const char *operation)
{
	if (XR_SUCCEEDED(result))
		return 1;
	xr_errorf("%s failed (OpenXR result %d).", operation, (int)result);
	host_logf(HOST_LOG_WARN, "OpenXR %s failed: %d", operation, (int)result);
	return 0;
}

static int load_global(const char *name, PFN_xrVoidFunction *function)
{
	XrResult result = p_xrGetInstanceProcAddr(XR_NULL_HANDLE, name, function);
	if (XR_FAILED(result) || !*function)
	{
		xr_errorf("The OpenXR loader has no %s (result %d).", name, (int)result);
		host_logf(HOST_LOG_WARN, "OpenXR loader has no %s (%d)", name, (int)result);
		return 0;
	}
	return 1;
}

static int load_instance(const char *name, PFN_xrVoidFunction *function)
{
	XrResult result = p_xrGetInstanceProcAddr(xr.instance, name, function);
	if (XR_FAILED(result) || !*function)
	{
		xr_errorf("The OpenXR runtime has no %s (result %d).", name, (int)result);
		host_logf(HOST_LOG_WARN, "OpenXR runtime has no %s (%d)", name, (int)result);
		return 0;
	}
	return 1;
}

#define LOAD_INSTANCE(name) \
	do { if (!load_instance(#name, (PFN_xrVoidFunction *)&p_##name)) return 0; } while (0)

static const struct touch_binding
{
	const char *name, *path;
	XrActionType type;
	int slot;
} touch_bindings[] = {
	{ "move", "/user/hand/left/input/thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, 0 },
	{ "look", "/user/hand/right/input/thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, 2 },
	{ "grenade", "/user/hand/left/input/trigger/value", XR_ACTION_TYPE_FLOAT_INPUT, 4 },
	{ "fire", "/user/hand/right/input/trigger/value", XR_ACTION_TYPE_FLOAT_INPUT, 5 },
	{ "accept_jump", "/user/hand/right/input/a/click", XR_ACTION_TYPE_BOOLEAN_INPUT, 6 },
	{ "back_melee", "/user/hand/right/input/b/click", XR_ACTION_TYPE_BOOLEAN_INPUT, 7 },
	{ "use_reload", "/user/hand/left/input/x/click", XR_ACTION_TYPE_BOOLEAN_INPUT, 8 },
	{ "switch_grenade", "/user/hand/left/input/y/click", XR_ACTION_TYPE_BOOLEAN_INPUT, 9 },
	{ "pause", "/user/hand/left/input/menu/click", XR_ACTION_TYPE_BOOLEAN_INPUT, 10 },
	{ "crouch", "/user/hand/left/input/thumbstick/click", XR_ACTION_TYPE_BOOLEAN_INPUT, 11 },
	{ "zoom", "/user/hand/right/input/thumbstick/click", XR_ACTION_TYPE_BOOLEAN_INPUT, 12 },
	{ "flashlight", "/user/hand/left/input/squeeze/value", XR_ACTION_TYPE_FLOAT_INPUT, 13 },
	{ "switch_weapon_grip", "/user/hand/right/input/squeeze/value", XR_ACTION_TYPE_FLOAT_INPUT, 14 },
	{ "aim_left", "/user/hand/left/input/aim/pose", XR_ACTION_TYPE_POSE_INPUT, -1 },
	{ "aim_right", "/user/hand/right/input/aim/pose", XR_ACTION_TYPE_POSE_INPUT, -1 },
	{ "haptic_left", "/user/hand/left/output/haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, -1 },
	{ "haptic_right", "/user/hand/right/output/haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, -1 },
};

static int create_actions(void)
{
	XrActionSetCreateInfo set = { XR_TYPE_ACTION_SET_CREATE_INFO };
	XrActionSuggestedBinding bindings[XR_ACTION_COUNT];
	XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	unsigned i;

	strcpy(set.actionSetName, "halo");
	strcpy(set.localizedActionSetName, "Halo controls");
	if (!xr_ok(p_xrCreateActionSet(xr.instance, &set, &xr.action_set), "create Touch action set"))
		return 0;
	for (i = 0; i < XR_ACTION_COUNT; ++i)
	{
		XrActionCreateInfo action = { XR_TYPE_ACTION_CREATE_INFO };
		strcpy(action.actionName, touch_bindings[i].name);
		strcpy(action.localizedActionName, touch_bindings[i].name);
		action.actionType = touch_bindings[i].type;
		if (!xr_ok(p_xrCreateAction(xr.action_set, &action, &xr.actions[i]), "create Touch action") ||
			!xr_ok(p_xrStringToPath(xr.instance, touch_bindings[i].path, &bindings[i].binding), "Touch binding path"))
			return 0;
		bindings[i].action = xr.actions[i];
	}
	if (!xr_ok(p_xrStringToPath(xr.instance, "/interaction_profiles/oculus/touch_controller",
		&suggested.interactionProfile), "Touch profile path"))
		return 0;
	suggested.countSuggestedBindings = XR_ACTION_COUNT;
	suggested.suggestedBindings = bindings;
	if (!xr_ok(p_xrSuggestInteractionProfileBindings(xr.instance, &suggested), "bind Touch controls"))
		return 0;
	attach.countActionSets = 1;
	attach.actionSets = &xr.action_set;
	if (!xr_ok(p_xrAttachSessionActionSets(xr.session, &attach), "attach Touch controls"))
		return 0;
	for (i = 0; i < POSE_ACTION_COUNT; ++i)
	{
		XrActionSpaceCreateInfo space = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		space.action = xr.actions[TOUCH_ACTION_COUNT + i];
		space.poseInActionSpace.orientation.w = 1.0f;
		if (!xr_ok(p_xrCreateActionSpace(xr.session, &space, &xr.controller_spaces[i]),
			"create Touch aim space"))
			return 0;
	}
	return 1;
}

static void sync_actions(void)
{
	float state[15] = {0};
	XrActiveActionSet active = { xr.action_set, XR_NULL_PATH };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	unsigned i;

	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (xr.session_running && xr.session_state == XR_SESSION_STATE_FOCUSED &&
		p_xrSyncActions(xr.session, &sync) == XR_SUCCESS)
	{
		for (i = 0; i < TOUCH_ACTION_COUNT; ++i)
		{
			XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
			int slot = touch_bindings[i].slot;
			get.action = xr.actions[i];
			if (touch_bindings[i].type == XR_ACTION_TYPE_BOOLEAN_INPUT)
			{
				XrActionStateBoolean value = { XR_TYPE_ACTION_STATE_BOOLEAN };
				if (XR_SUCCEEDED(p_xrGetActionStateBoolean(xr.session, &get, &value)) && value.isActive)
					state[slot] = value.currentState ? 1.0f : 0.0f;
			}
			else if (touch_bindings[i].type == XR_ACTION_TYPE_FLOAT_INPUT)
			{
				XrActionStateFloat value = { XR_TYPE_ACTION_STATE_FLOAT };
				if (XR_SUCCEEDED(p_xrGetActionStateFloat(xr.session, &get, &value)) && value.isActive)
					state[slot] = value.currentState;
			}
			else
			{
				XrActionStateVector2f value = { XR_TYPE_ACTION_STATE_VECTOR2F };
				if (XR_SUCCEEDED(p_xrGetActionStateVector2f(xr.session, &get, &value)) && value.isActive)
				{
					state[slot] = value.currentState.x;
					state[slot + 1] = value.currentState.y;
				}
			}
		}
	}
	pthread_mutex_lock(&touch_lock);
	memcpy(touch_state, state, sizeof(state));
	pthread_mutex_unlock(&touch_lock);
}

static void locate_controller_poses(void)
{
	struct xr_controller_pose poses[POSE_ACTION_COUNT] = {0};
	float dt = 0.0f;
	unsigned i;

	if (xr.session_running && xr.session_state == XR_SESSION_STATE_FOCUSED &&
		xr.space != XR_NULL_HANDLE)
	{
		for (i = 0; i < POSE_ACTION_COUNT; ++i)
		{
			XrSpaceVelocity velocity = { XR_TYPE_SPACE_VELOCITY };
			XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION, &velocity };
			if (XR_SUCCEEDED(p_xrLocateSpace(xr.controller_spaces[i], xr.space,
				xr.predicted_time, &location)) &&
				(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
				(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			{
				poses[i].position[0] = location.pose.position.x;
				poses[i].position[1] = location.pose.position.y;
				poses[i].position[2] = location.pose.position.z;
				poses[i].orientation[0] = location.pose.orientation.x;
				poses[i].orientation[1] = location.pose.orientation.y;
				poses[i].orientation[2] = location.pose.orientation.z;
				poses[i].orientation[3] = location.pose.orientation.w;
				poses[i].valid = 1;
				if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
				{
					poses[i].linear_velocity[0] = velocity.linearVelocity.x;
					poses[i].linear_velocity[1] = velocity.linearVelocity.y;
					poses[i].linear_velocity[2] = velocity.linearVelocity.z;
					poses[i].velocity_valid = 1;
				}
			}
		}
	}
	pthread_mutex_lock(&touch_lock);
	if (controller_filter_time && xr.predicted_time > controller_filter_time)
		dt = (float)(xr.predicted_time - controller_filter_time) / 1000000000.0f;
	for (i = 0; i < POSE_ACTION_COUNT; ++i)
		smooth_controller_pose(&controller_poses[i], &poses[i], dt);
	controller_filter_time = xr.predicted_time;
	head_position_valid = xr.session_running && xr.session_state == XR_SESSION_STATE_FOCUSED &&
		xr.view_count == EYE_COUNT;
	if (head_position_valid)
	{
		head_position[0] = (xr.views[0].pose.position.x + xr.views[1].pose.position.x) * 0.5f;
		head_position[1] = (xr.views[0].pose.position.y + xr.views[1].pose.position.y) * 0.5f;
		head_position[2] = (xr.views[0].pose.position.z + xr.views[1].pose.position.z) * 0.5f;
		head_orientation[0] = xr.views[0].pose.orientation.x;
		head_orientation[1] = xr.views[0].pose.orientation.y;
		head_orientation[2] = xr.views[0].pose.orientation.z;
		head_orientation[3] = xr.views[0].pose.orientation.w;
	}
	pthread_mutex_unlock(&touch_lock);
}

static int create_swapchains(void)
{
	XrViewConfigurationView view_configs[EYE_COUNT];
	int64_t *formats = NULL;
	uint32_t view_count = 0, format_count = 0, i, j;
	int64_t format = GL_RGBA8;

	memset(view_configs, 0, sizeof(view_configs));
	for (i = 0; i < EYE_COUNT; i++)
		view_configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	if (!xr_ok(p_xrEnumerateViewConfigurationViews(xr.instance, xr.system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, EYE_COUNT, &view_count, view_configs),
		"enumerate stereo views") || view_count != EYE_COUNT)
		return 0;
	if (!xr_ok(p_xrEnumerateSwapchainFormats(xr.session, 0, &format_count, NULL),
		"count swapchain formats") || !format_count)
		return 0;
	formats = calloc(format_count, sizeof(*formats));
	if (!formats)
	{
		xr_errorf("Out of memory while reading OpenXR swapchain formats.");
		return 0;
	}
	if (!xr_ok(p_xrEnumerateSwapchainFormats(xr.session, format_count, &format_count, formats),
		"enumerate swapchain formats"))
	{
		free(formats);
		return 0;
	}
	format = formats[0];
	for (i = 0; i < format_count; i++)
	{
		if (formats[i] == GL_SRGB8_ALPHA8)
		{
			format = formats[i];
			break;
		}
		if (formats[i] == GL_RGBA8)
			format = formats[i];
	}
	free(formats);

	for (i = 0; i < EYE_COUNT; i++)
	{
		XrSwapchainCreateInfo create_info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		XrSwapchainCreateInfoFoveationFB foveation_info = {
			XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB };
		struct xr_eye *eye = &xr.eyes[i];

		eye->width = view_configs[i].recommendedImageRectWidth;
		eye->height = view_configs[i].recommendedImageRectHeight;
		create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
			XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		create_info.format = format;
		create_info.sampleCount = 1;
		create_info.width = eye->width;
		create_info.height = eye->height;
		create_info.faceCount = 1;
		create_info.arraySize = 1;
		create_info.mipCount = 1;
		create_info.next = &foveation_info;
		if (!xr_ok(p_xrCreateSwapchain(xr.session, &create_info, &eye->swapchain),
			"create eye swapchain"))
			return 0;
		if (xr.foveation_profile != XR_NULL_HANDLE)
		{
			XrSwapchainStateFoveationFB state = {
				XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB };
			state.profile = xr.foveation_profile;
			if (!xr_ok(p_xrUpdateSwapchainFB(eye->swapchain,
				(XrSwapchainStateBaseHeaderFB const *)&state),
				"enable eye foveation"))
				return 0;
		}
		if (!xr_ok(p_xrEnumerateSwapchainImages(eye->swapchain, 0, &eye->image_count, NULL),
			"count swapchain images"))
			return 0;
		eye->images = calloc(eye->image_count, sizeof(*eye->images));
		if (!eye->images)
		{
			xr_errorf("Out of memory while reading OpenXR eye images.");
			return 0;
		}
		for (j = 0; j < eye->image_count; j++)
			eye->images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
		if (!xr_ok(p_xrEnumerateSwapchainImages(eye->swapchain, eye->image_count,
			&eye->image_count, (XrSwapchainImageBaseHeader *)eye->images),
			"enumerate swapchain images"))
			return 0;
	}
	host_logf(HOST_LOG_INFO, "OpenXR eye swapchains: %ux%u and %ux%u, GL format 0x%llx",
		xr.eyes[0].width, xr.eyes[0].height, xr.eyes[1].width, xr.eyes[1].height,
		(unsigned long long)format);
	return 1;
}

int host_xr_initialize(void)
{
	JNIEnv *environment;
	JavaVM *vm = NULL;
	jobject activity;
	XrLoaderInitInfoAndroidKHR loader_info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	XrInstanceCreateInfo instance_info = { XR_TYPE_INSTANCE_CREATE_INFO };
	XrSystemGetInfo system_info = { XR_TYPE_SYSTEM_GET_INFO };
	XrGraphicsRequirementsOpenGLESKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	XrSessionCreateInfo session_info = { XR_TYPE_SESSION_CREATE_INFO };
	XrReferenceSpaceCreateInfo space_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	const char *extensions[] = {
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME,
		XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME,
		XR_FB_FOVEATION_EXTENSION_NAME,
		XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME,
	};
	EGLint config_id = 0, count = 0;
	EGLint attributes[3];

	if (xr.initialized)
		return xr.session != XR_NULL_HANDLE;
	xr.initialized = 1;
	xr.loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
	if (!xr.loader)
	{
		const char *error = dlerror();

		xr_errorf("The packaged OpenXR loader could not be opened: %s", error ? error : "unknown error");
		host_logf(HOST_LOG_WARN, "OpenXR loader unavailable: %s", error ? error : "unknown error");
		return 0;
	}
	p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(xr.loader, "xrGetInstanceProcAddr");
	if (!p_xrGetInstanceProcAddr)
	{
		xr_errorf("The packaged OpenXR loader has no xrGetInstanceProcAddr.");
		return 0;
	}
	if (!load_global("xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&p_xrInitializeLoaderKHR))
		return 0;

	environment = (JNIEnv *)SDL_GetAndroidJNIEnv();
	activity = (jobject)SDL_GetAndroidActivity();
	if (!environment || !activity || (*environment)->GetJavaVM(environment, &vm) != JNI_OK)
	{
		xr_errorf("SDL did not provide the Android activity and Java VM.");
		host_logf(HOST_LOG_WARN, "OpenXR could not obtain the Android activity/VM");
		return 0;
	}
	loader_info.applicationVM = vm;
	loader_info.applicationContext = activity;
	if (!xr_ok(p_xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR *)&loader_info),
		"initialize loader"))
		return 0;
	if (!load_global("xrCreateInstance", (PFN_xrVoidFunction *)&p_xrCreateInstance))
		return 0;

	android_info.applicationVM = vm;
	android_info.applicationActivity = activity;
	instance_info.next = &android_info;
	strncpy(instance_info.applicationInfo.applicationName, "AlphaHaloQuest",
		XR_MAX_APPLICATION_NAME_SIZE - 1);
	strncpy(instance_info.applicationInfo.engineName, "Halo CE Universal",
		XR_MAX_ENGINE_NAME_SIZE - 1);
	instance_info.applicationInfo.applicationVersion = 1;
	instance_info.applicationInfo.engineVersion = 1;
	instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instance_info.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
	instance_info.enabledExtensionNames = extensions;
	if (!xr_ok(p_xrCreateInstance(&instance_info, &xr.instance), "create instance"))
		return 0;

	LOAD_INSTANCE(xrDestroyInstance);
	LOAD_INSTANCE(xrGetSystem);
	LOAD_INSTANCE(xrGetOpenGLESGraphicsRequirementsKHR);
	LOAD_INSTANCE(xrCreateSession);
	LOAD_INSTANCE(xrDestroySession);
	LOAD_INSTANCE(xrCreateReferenceSpace);
	LOAD_INSTANCE(xrDestroySpace);
	LOAD_INSTANCE(xrEnumerateViewConfigurationViews);
	LOAD_INSTANCE(xrEnumerateSwapchainFormats);
	LOAD_INSTANCE(xrCreateSwapchain);
	LOAD_INSTANCE(xrDestroySwapchain);
	LOAD_INSTANCE(xrEnumerateSwapchainImages);
	LOAD_INSTANCE(xrPollEvent);
	LOAD_INSTANCE(xrBeginSession);
	LOAD_INSTANCE(xrEndSession);
	LOAD_INSTANCE(xrWaitFrame);
	LOAD_INSTANCE(xrBeginFrame);
	LOAD_INSTANCE(xrEndFrame);
	LOAD_INSTANCE(xrLocateViews);
	LOAD_INSTANCE(xrAcquireSwapchainImage);
	LOAD_INSTANCE(xrWaitSwapchainImage);
	LOAD_INSTANCE(xrReleaseSwapchainImage);
	LOAD_INSTANCE(xrStringToPath);
	LOAD_INSTANCE(xrCreateActionSet);
	LOAD_INSTANCE(xrCreateAction);
	LOAD_INSTANCE(xrSuggestInteractionProfileBindings);
	LOAD_INSTANCE(xrAttachSessionActionSets);
	LOAD_INSTANCE(xrSyncActions);
	LOAD_INSTANCE(xrGetActionStateBoolean);
	LOAD_INSTANCE(xrGetActionStateFloat);
	LOAD_INSTANCE(xrGetActionStateVector2f);
	LOAD_INSTANCE(xrCreateActionSpace);
	LOAD_INSTANCE(xrLocateSpace);
	LOAD_INSTANCE(xrApplyHapticFeedback);
	LOAD_INSTANCE(xrPerfSettingsSetPerformanceLevelEXT);
	LOAD_INSTANCE(xrCreateFoveationProfileFB);
	LOAD_INSTANCE(xrDestroyFoveationProfileFB);
	LOAD_INSTANCE(xrUpdateSwapchainFB);

	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!xr_ok(p_xrGetSystem(xr.instance, &system_info, &xr.system), "get HMD system"))
		return 0;
	if (!xr_ok(p_xrGetOpenGLESGraphicsRequirementsKHR(xr.instance, xr.system, &requirements),
		"get OpenGL ES requirements"))
		return 0;

	binding.display = eglGetCurrentDisplay();
	binding.context = eglGetCurrentContext();
	if (binding.display == EGL_NO_DISPLAY || binding.context == EGL_NO_CONTEXT ||
		!eglQueryContext(binding.display, binding.context, EGL_CONFIG_ID, &config_id))
	{
		xr_errorf("No current EGL display/context/configuration was available.");
		host_logf(HOST_LOG_WARN, "OpenXR has no current EGL context/configuration");
		return 0;
	}
	attributes[0] = EGL_CONFIG_ID;
	attributes[1] = config_id;
	attributes[2] = EGL_NONE;
	if (!eglChooseConfig(binding.display, attributes, &binding.config, 1, &count) || count != 1)
	{
		xr_errorf("The current EGL configuration %d could not be resolved.", config_id);
		host_logf(HOST_LOG_WARN, "OpenXR could not resolve EGL config %d", config_id);
		return 0;
	}
	session_info.next = &binding;
	session_info.systemId = xr.system;
	if (!xr_ok(p_xrCreateSession(xr.instance, &session_info, &xr.session), "create session"))
		return 0;
	/* Ask Quest for a stable sustained clock rather than short boost bursts.
	   This reduces frame-time spikes without forcing a higher refresh rate that
	   the game may not hold continuously. */
	p_xrPerfSettingsSetPerformanceLevelEXT(xr.session,
		XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
	p_xrPerfSettingsSetPerformanceLevelEXT(xr.session,
		XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
	{
		XrFoveationLevelProfileCreateInfoFB level = {
			XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB };
		XrFoveationProfileCreateInfoFB profile = {
			XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB };

		/* Dynamic medium foveation saves fill rate at the lens periphery while
		   preserving the centre used for aiming and text. Quest can lower the
		   level automatically when GPU headroom is available. */
		level.level = XR_FOVEATION_LEVEL_MEDIUM_FB;
		level.dynamic = XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB;
		profile.next = &level;
		if (!xr_ok(p_xrCreateFoveationProfileFB(xr.session, &profile,
			&xr.foveation_profile), "create foveation profile"))
			return 0;
	}
	if (!create_actions())
		return 0;
	if (!create_swapchains())
		return 0;

	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	if (!xr_ok(p_xrCreateReferenceSpace(xr.session, &space_info, &xr.space),
		"create local reference space"))
		return 0;
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	memset(&space_info.poseInReferenceSpace, 0, sizeof(space_info.poseInReferenceSpace));
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	if (!xr_ok(p_xrCreateReferenceSpace(xr.session, &space_info, &xr.view_space),
		"create view reference space"))
		return 0;
	for (count = 0; count < EYE_COUNT; count++)
		xr.views[count].type = XR_TYPE_VIEW;
	glGenFramebuffers(1, &xr.framebuffer);
	xr.session_created_ticks = SDL_GetTicks();
	host_logf(HOST_LOG_INFO, "OpenXR Quest session created");
	return 1;
}

static void poll_events(void)
{
	XrEventDataBuffer event;

	for (;;)
	{
		memset(&event, 0, sizeof(event));
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		if (p_xrPollEvent(xr.instance, &event) != XR_SUCCESS)
			break;
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			XrEventDataSessionStateChanged *changed = (XrEventDataSessionStateChanged *)&event;
			xr.session_state = changed->state;
			if (changed->state == XR_SESSION_STATE_READY && !xr.session_running)
			{
				XrSessionBeginInfo begin_info = { XR_TYPE_SESSION_BEGIN_INFO };
				begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (xr_ok(p_xrBeginSession(xr.session, &begin_info), "begin session"))
				{
					xr.session_running = 1;
					xr.ever_running = 1;
				}
			}
			else if (changed->state == XR_SESSION_STATE_STOPPING && xr.session_running)
			{
				p_xrEndSession(xr.session);
				xr.session_running = 0;
			}
			else if (changed->state == XR_SESSION_STATE_EXITING ||
				changed->state == XR_SESSION_STATE_LOSS_PENDING)
				xr.session_running = 0;
		}
	}
}

int host_xr_active(void)
{
	return xr.session != XR_NULL_HANDLE && xr.session_running;
}

int host_xr_begin_frame(void)
{
	XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frame_state = { XR_TYPE_FRAME_STATE };
	XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
	XrViewLocateInfo locate_info = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState view_state = { XR_TYPE_VIEW_STATE };
	uint32_t count = 0;

	if (!xr.session)
		return 0;
	poll_events();
	sync_actions();
	if (!xr.session_running)
	{
		locate_controller_poses();
		if (!xr.ever_running && SDL_GetTicks() - xr.session_created_ticks > 10000)
			host_fatal("OpenXR created a session but Quest never gave it immersive VR focus.\n\n"
				"Session state: %d. The activity was launched with both Meta and Khronos VR categories.",
				(int)xr.session_state);
		return 0;
	}
	if (xr.frame_begun)
		return 1;
	if (!xr_ok(p_xrWaitFrame(xr.session, &wait_info, &frame_state), "wait frame") ||
		!xr_ok(p_xrBeginFrame(xr.session, &begin_info), "begin frame"))
		return 0;
	xr.frame_begun = 1;
	xr.should_render = frame_state.shouldRender;
	xr.predicted_time = frame_state.predictedDisplayTime;
	xr.stereo = 0;
	locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate_info.displayTime = xr.predicted_time;
	locate_info.space = xr.space;
	if (!xr_ok(p_xrLocateViews(xr.session, &locate_info, &view_state, EYE_COUNT,
		&count, xr.views), "locate views"))
		count = 0;
	xr.view_count = count;
	if (!(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) ||
		!(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
		xr.view_count = 0;
	if (count == EYE_COUNT && !xr.recentered &&
		(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
		(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
	{
		XrReferenceSpaceCreateInfo space_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		XrSpace recentered_space = XR_NULL_HANDLE;

		/* Define the game's origin at the initial head centre. Rendering and
		projection poses then share a coordinate system, keeping runtime
		timewarp correct. */
		space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		/* Recenter yaw only: preserve gravity even if startup occurs while
		looking down. */
		{
			XrQuaternionf q = xr.views[0].pose.orientation;
			float yaw = atan2f(2.0f * (q.w*q.y + q.x*q.z),
				1.0f - 2.0f * (q.x*q.x + q.y*q.y));
			space_info.poseInReferenceSpace.orientation.y = sinf(yaw * 0.5f);
			space_info.poseInReferenceSpace.orientation.w = cosf(yaw * 0.5f);
		}
		space_info.poseInReferenceSpace.position.x =
			(xr.views[0].pose.position.x + xr.views[1].pose.position.x) * 0.5f;
		space_info.poseInReferenceSpace.position.y =
			(xr.views[0].pose.position.y + xr.views[1].pose.position.y) * 0.5f;
		space_info.poseInReferenceSpace.position.z =
			(xr.views[0].pose.position.z + xr.views[1].pose.position.z) * 0.5f;
		if (xr_ok(p_xrCreateReferenceSpace(xr.session, &space_info, &recentered_space),
			"recenter local space"))
		{
			p_xrDestroySpace(xr.space);
			xr.space = recentered_space;
			locate_info.space = xr.space;
			if (xr_ok(p_xrLocateViews(xr.session, &locate_info, &view_state, EYE_COUNT,
				&count, xr.views), "relocate recentered views"))
				xr.view_count = count;
			xr.recentered = 1;
		}
	}
	locate_controller_poses();
	return 1;
}

void host_xr_set_stereo(int stereo)
{
	xr.stereo = stereo != 0;
}

void host_xr_set_source_dimensions(int width, int height)
{
	source_width = width > 0 ? width : 0;
	source_height = height > 0 ? height : 0;
}

int host_xr_get_view(int eye, float *position, float *orientation, float *fov)
{
	XrView *view;
	if (eye < 0 || eye >= EYE_COUNT || xr.view_count != EYE_COUNT || !xr.recentered ||
		!position || !orientation || !fov)
		return 0;
	view = &xr.views[eye];
	position[0] = view->pose.position.x;
	position[1] = view->pose.position.y;
	position[2] = view->pose.position.z;
	orientation[0] = view->pose.orientation.x;
	orientation[1] = view->pose.orientation.y;
	orientation[2] = view->pose.orientation.z;
	orientation[3] = view->pose.orientation.w;
	/* The guest constructs the same asymmetric projection. Returning the
	runtime angles here keeps rendering and compositor reprojection identical. */
	fov[0] = view->fov.angleLeft;
	fov[1] = view->fov.angleRight;
	fov[2] = view->fov.angleUp;
	fov[3] = view->fov.angleDown;
	return 1;
}

int host_xr_end_frame(int drawable_width, int drawable_height)
{
	XrCompositionLayerProjectionView projection_views[EYE_COUNT];
	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad menu = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerBaseHeader *layers[1];
	XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
	GLint old_read = 0, old_draw = 0;
	GLboolean old_scissor;
	int content_x = 0, content_y = 0, content_width, content_height;
	int direct_source = 0;
	uint32_t i, rendered = 0;

	if (!xr.session)
		return 0;
	if (!xr.frame_begun && !host_xr_begin_frame())
		return 0;
	memset(projection_views, 0, sizeof(projection_views));
	if (xr.should_render && xr.view_count == EYE_COUNT && drawable_width > 0 && drawable_height > 0)
	{
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
		/* Prefer d3d8_gl's still-bound full-resolution backbuffer. The old path
		copied the Android window after a 1920x960 -> 1440x720 downscale, then
		upscaled each 720-square half into a Quest eye texture. */
		direct_source = old_read != 0 && source_width > 0 && source_height > 0;
		if (direct_source)
		{
			content_width = source_width;
			content_height = source_height;
		}
		else
		{
			/* Fallback for any presentation path that did not identify an engine
			backbuffer: use the letterboxed SDL default framebuffer. */
			content_width = drawable_width;
			content_height = drawable_width / 2;
			if (content_height > drawable_height)
			{
				content_height = drawable_height;
				content_width = drawable_height * 2;
			}
			content_x = (drawable_width - content_width) / 2;
			content_y = (drawable_height - content_height) / 2;
		}
		old_scissor = glIsEnabled(GL_SCISSOR_TEST);
		glDisable(GL_SCISSOR_TEST);
		for (i = 0; i < (xr.stereo ? EYE_COUNT : 1); i++)
		{
			struct xr_eye *eye = &xr.eyes[i];
			XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			uint32_t image_index = 0;
			int source_x = content_x;
			int source_width = content_width;

			wait_info.timeout = XR_INFINITE_DURATION;
			if (!xr_ok(p_xrAcquireSwapchainImage(eye->swapchain, &acquire_info, &image_index),
				"acquire eye image") ||
				!xr_ok(p_xrWaitSwapchainImage(eye->swapchain, &wait_info), "wait eye image"))
				break;
			if (xr.stereo)
			{
				source_width = content_width / 2;
				source_x += (int)i * source_width;
			}
			glBindFramebuffer(GL_READ_FRAMEBUFFER, direct_source ? (GLuint)old_read : 0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, xr.framebuffer);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				eye->images[image_index].image, 0);
			if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				host_fatal("OpenXR eye framebuffer is incomplete (eye %u).", i);
			/* D3D render-target row zero is the top, so direct presentation needs
			the same vertical flip as the ordinary window blit. */
			glBlitFramebuffer(source_x,
				direct_source ? content_y + content_height : content_y,
				source_x + source_width,
				direct_source ? content_y : content_y + content_height,
				0, 0, (GLint)eye->width, (GLint)eye->height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			glFlush();
			p_xrReleaseSwapchainImage(eye->swapchain, &release_info);

			projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projection_views[i].pose = xr.views[i].pose;
			projection_views[i].fov = xr.views[i].fov;
			projection_views[i].subImage.swapchain = eye->swapchain;
			projection_views[i].subImage.imageRect.offset.x = 0;
			projection_views[i].subImage.imageRect.offset.y = 0;
			projection_views[i].subImage.imageRect.extent.width = eye->width;
			projection_views[i].subImage.imageRect.extent.height = eye->height;
			rendered++;
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_draw);
		if (old_scissor)
			glEnable(GL_SCISSOR_TEST);
	}

	layer.space = xr.space;
	layer.viewCount = EYE_COUNT;
	layer.views = projection_views;
	layers[0] = (XrCompositionLayerBaseHeader *)&layer;
	if (!xr.stereo)
	{
		/* One physical screen, two eyes. The runtime supplies correct
		perspective and IPD; it stays in the room as the head moves. */
		/* Menus are head-locked. A local-space quad remains at the original room
		   heading, which looks like a black screen after the player has turned. */
		menu.space = xr.view_space;
		menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		menu.subImage = projection_views[0].subImage;
		menu.pose.orientation.w = 1.0f;
		menu.pose.position.z = -2.0f;
		menu.size.width = 2.4f;
		menu.size.height = 1.2f;
		layers[0] = (XrCompositionLayerBaseHeader *)&menu;
	}
	end_info.displayTime = xr.predicted_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = rendered == (xr.stereo ? EYE_COUNT : 1) ? 1 : 0;
	end_info.layers = end_info.layerCount ? (const XrCompositionLayerBaseHeader *const *)layers : NULL;
	if (!xr_ok(p_xrEndFrame(xr.session, &end_info), "submit frame"))
		host_fatal("OpenXR presentation failed: %s", host_xr_last_error());
	xr.frame_begun = 0;
	source_width = 0;
	source_height = 0;
	return 1;
}
