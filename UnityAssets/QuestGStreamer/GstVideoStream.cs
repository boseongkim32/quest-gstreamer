using System;
using System.Runtime.InteropServices;
using UnityEngine;

public class GstVideoStream : MonoBehaviour
{
    [DllImport("gst_unity_plugin")]
    private static extern int GstUnityPlugin_Init();

    [DllImport("gst_unity_plugin")]
    private static extern int GstUnityPlugin_StartStream(int port, string codecName, string mime,
                                                          int expectedFrameRate);

    [DllImport("gst_unity_plugin")]
    private static extern void GstUnityPlugin_StopStream();

    [DllImport("gst_unity_plugin")]
    private static extern void GstUnityPlugin_Shutdown();

    [DllImport("gst_unity_plugin")]
    private static extern int GstUnityPlugin_GetStreamState();

    [DllImport("gst_unity_plugin")]
    private static extern int GstUnityPlugin_GetLastErrorCode();

    [DllImport("gst_unity_plugin")]
    private static extern void GstUnityPlugin_ListDecoders();

    [DllImport("gst_unity_plugin")]
    private static extern void GstUnityPlugin_InspectDecoder(string decoderName);

    [DllImport("gst_unity_plugin")]
    private static extern IntPtr GstUnityPlugin_GetRenderEventFunc();

    [DllImport("gst_unity_plugin")]
    private static extern void GstUnityPlugin_SetTexture(IntPtr rgbaTexture, int width, int height);

    public enum NativeStreamState
    {
        Stopped = 0,
        Starting = 1,
        Streaming = 2,
        Stalled = 3,
        StreamLost = Stalled,
        Failed = 4
    }

    public enum NativeErrorCode
    {
        None = 0,
        SourceStartFailed = 1,
        SampleMissingBuffer = 2,
        SampleMissingCaps = 3,
        DecoderCreateFailed = 4,
        DecoderStartFailed = 5,
        DecoderAsyncError = 6,
        SampleMissingResolution = 7,
        UnsupportedFormatChange = 8,
        RendererFailed = 9
    }

    [SerializeField] private int port = 5000;
    [SerializeField] private int textureWidth = 1280;
    [SerializeField] private int textureHeight = 720;
    [SerializeField] private Renderer targetRenderer;

    [Header("Decoder Configuration")]
    [Tooltip("MediaCodec name (e.g. 'c2.qti.avc.decoder.low_latency'). Leave empty for generic fallback.")]
    [SerializeField] private string decoderCodecName = "c2.qti.avc.decoder.low_latency";
    [Tooltip("MIME type: 'video/avc' or 'video/hevc'")]
    [SerializeField] private string decoderMime = "video/avc";
    [Tooltip("Expected incoming framerate — sets AMEDIAFORMAT_KEY_FRAME_RATE (0 = let decoder default)")]
    [SerializeField] private int expectedFrameRate = 60;

    private RenderTexture rgbaTexture;
    private IntPtr renderEventFunc;
    private bool nativeInitialized = false;
    private bool streamStarted = false;
    private bool hasEverStreamed = false;
    private NativeStreamState currentState = NativeStreamState.Stopped;
    private NativeErrorCode lastErrorCode = NativeErrorCode.None;

    public NativeStreamState CurrentState => currentState;
    public NativeErrorCode LastErrorCode => lastErrorCode;
    public bool HasFailed => currentState == NativeStreamState.Failed;
    public bool IsStalled => currentState == NativeStreamState.Stalled;
    public bool StreamLost => IsStalled;
    public bool IsWaitingForFirstFrame =>
        streamStarted && currentState == NativeStreamState.Starting && !IsStalled && !HasFailed;

    // Polls native state once. Calls into GetStreamState also drive any pending
    // terminal cleanup on the native side (see FinalizeDeferredCleanup),
    // so this single call per frame is the only thing Unity has to do.
    private void PollNativeState()
    {
        if (!nativeInitialized) return;

        NativeStreamState newState = (NativeStreamState)GstUnityPlugin_GetStreamState();
        lastErrorCode = (NativeErrorCode)GstUnityPlugin_GetLastErrorCode();

        if (newState != currentState)
        {
            OnStateChanged(currentState, newState);
            currentState = newState;
        }

        if (currentState == NativeStreamState.Streaming)
        {
            hasEverStreamed = true;
        }
    }

    private void OnStateChanged(NativeStreamState from, NativeStreamState to)
    {
        switch (to)
        {
            case NativeStreamState.Streaming:
                if (from == NativeStreamState.Starting)
                {
                    Debug.Log("[GstVideoStream] Stream is now streaming.");
                }
                break;
            case NativeStreamState.Stalled:
                Debug.LogWarning("[GstVideoStream] Native stream stalled after samples stopped arriving. Call RestartStream() to resume playback.");
                break;
            case NativeStreamState.Failed:
                LogNativeFailure();
                break;
        }
    }

    private void LogNativeFailure()
    {
        if (lastErrorCode == NativeErrorCode.UnsupportedFormatChange)
        {
            Debug.LogError("[GstVideoStream] Native stream failed: unsupported mid-stream resolution/format change. Restart the stream session after the sender is producing a fixed resolution again.");
            return;
        }

        Debug.LogError($"[GstVideoStream] Native stream failed: {lastErrorCode}. Call RestartStream() to try again.");
    }

    private void ReleaseTexture()
    {
        if (rgbaTexture == null) return;

        rgbaTexture.Release();
        Destroy(rgbaTexture);
        rgbaTexture = null;
    }

    private void ShutdownPlugin()
    {
        if (streamStarted)
        {
            GstUnityPlugin_StopStream();
            streamStarted = false;
        }

        if (nativeInitialized)
        {
            GstUnityPlugin_Shutdown();
            nativeInitialized = false;
        }

        renderEventFunc = IntPtr.Zero;
        hasEverStreamed = false;
        currentState = NativeStreamState.Stopped;
        lastErrorCode = NativeErrorCode.None;
    }

    private bool StartNativeStream()
    {
        // Don't pre-set currentState; PollNativeState below picks up the
        // actual native state once StartStream returns.
        lastErrorCode = NativeErrorCode.None;
        hasEverStreamed = false;

        int result = GstUnityPlugin_StartStream(
            port,
            decoderCodecName,
            decoderMime,
            expectedFrameRate);
        PollNativeState();
        if (result != 0)
        {
            Debug.LogError($"Failed to start stream: {lastErrorCode}");
            return false;
        }

        streamStarted = true;
        Debug.Log($"GStreamer zero-copy stream started on port {port}");
        return true;
    }

    public bool RestartStream()
    {
        if (!nativeInitialized)
        {
            Debug.LogWarning("[GstVideoStream] Cannot restart stream before native plugin initialization succeeds.");
            return false;
        }

        if (renderEventFunc == IntPtr.Zero)
        {
            Debug.LogWarning("[GstVideoStream] Cannot restart stream without a render event function.");
            return false;
        }

        if (streamStarted || IsStalled || HasFailed)
        {
            GstUnityPlugin_StopStream();
            streamStarted = false;
        }

        hasEverStreamed = false;
        currentState = NativeStreamState.Stopped;
        lastErrorCode = NativeErrorCode.None;
        return StartNativeStream();
    }

    public void RestartStreamFromButton()
    {
        if (!RestartStream())
        {
            Debug.LogWarning("[GstVideoStream] Restart requested from UI, but restart did not complete.");
        }
    }

    void Start()
    {
        Debug.Log($"[GstVideoStream] Display refresh rate: {Screen.currentResolution.refreshRateRatio.value}Hz");

        int result = GstUnityPlugin_Init();
        if (result != 0)
        {
            Debug.LogError("Failed to initialize GStreamer");
            return;
        }
        nativeInitialized = true;

        rgbaTexture = new RenderTexture(textureWidth, textureHeight, 0, RenderTextureFormat.ARGB32);
        rgbaTexture.enableRandomWrite = true;
        rgbaTexture.filterMode = FilterMode.Bilinear;
        rgbaTexture.wrapMode = TextureWrapMode.Clamp;
        rgbaTexture.Create();

        if (targetRenderer != null)
        {
            targetRenderer.material.mainTexture = rgbaTexture;
        }
        else
        {
            Debug.LogWarning("Target renderer not set. Stream will be decoded but not displayed.");
        }

        renderEventFunc = GstUnityPlugin_GetRenderEventFunc();
        Debug.Log($"Render event func: {renderEventFunc}");
        if (renderEventFunc == IntPtr.Zero)
        {
            Debug.LogError("Failed to get render event function");
            ShutdownPlugin();
            ReleaseTexture();
            return;
        }

        GstUnityPlugin_SetTexture(
            rgbaTexture.GetNativeTexturePtr(),
            textureWidth,
            textureHeight);

        if (!StartNativeStream())
        {
            return;
        }
    }

    void Update()
    {
        PollNativeState();

        if (!streamStarted || IsStalled || HasFailed || renderEventFunc == IntPtr.Zero) return;

        if (currentState == NativeStreamState.Streaming)
        {
            GL.IssuePluginEvent(renderEventFunc, 0);
        }
    }

    void OnDestroy()
    {
        ShutdownPlugin();
        ReleaseTexture();
    }
}
