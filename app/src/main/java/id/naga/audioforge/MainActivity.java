package id.naga.audioforge;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

public class MainActivity extends AppCompatActivity {
    static { System.loadLibrary("audioforge_ndk"); }

    public native void nativeInitAudio();
    public native boolean nativeLoadFile(String path);
    public native boolean nativeAppendMergeFile(String path, int crossfadeMs);
    public native double nativeGetDurationMs();
    public native double nativeGetPlayheadMs();
    public native void nativeSetPlaybackRange(double startMs, double endMs, boolean play);
    public native void nativeScrubToMs(double targetMs, boolean audiblePreview);
    public native void nativeTogglePlay(boolean play);
    public native double nativeSnapZeroCrossingMs(double targetMs);
    public native float[] nativeGetWaveformPeaks(int numBins);
    public native float[] nativeGetZoomSamples(double centerMs, double windowMs, int numPoints);
    public native boolean nativeTrimKeepRange(double inMs, double outMs);
    public native boolean nativeRippleDeleteRange(double inMs, double outMs);
    public native int nativeStripSilence(float thresholdDb);
    public native float nativeNormalizeLufs(float targetLufs);
    public native void nativeDenoiseSpectral(float strength);
    public native void nativeApplyMasteringFx(int eqPreset, int fadeInMs, int fadeOutMs);
    public native void nativeMidSideProcess(int mode);
    public native void nativeApply8DSpatial();
    public native void nativeTimePitchProcess(float tempoRatio, float semitones);
    public native boolean nativeExportAudio(String outPath, int format, int sampleRate, int bitrateKbps, int channelMode);

    private static final int REQ_LOAD = 101, REQ_MERGE = 102;
    private PrecisionWaveformView waveView;
    private JogWheelView jogView;
    private TextView tvTele, tvStat;
    private double durMs = 0, posMs = 0, inMs = 0, outMs = 0;
    private int xfadeMs = 250, expFmt = 2, expRate = 48000, expBr = 256, expCh = 0;

    private final Handler h = new Handler(Looper.getMainLooper());
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (durMs > 0) { posMs = nativeGetPlayheadMs(); waveView.setPlayheadMs(posMs, null); updateTele(); }
            h.postDelayed(this, 40);
        }
    };

    @Override protected void onCreate(Bundle savedInstanceStateArg) {
        super.onCreate(savedInstanceStateArg);
        setContentView(R.layout.activity_main);
        nativeInitAudio();
        waveView = findViewById(R.id.waveformView);
        jogView = findViewById(R.id.jogWheelView);
        tvTele = findViewById(R.id.tvTelemetry);
        tvStat = findViewById(R.id.tvStatus);

        waveView.setOnScrubListener((ms, touch) -> {
            posMs = touch ? ms : nativeSnapZeroCrossingMs(ms);
            waveView.setPlayheadMs(posMs, nativeGetZoomSamples(posMs, 25.0, 120));
            nativeScrubToMs(posMs, touch); updateTele();
        });
        jogView.setOnJogDeltaListener(d -> { if (durMs > 0) nudge(d); });

        LinearLayout p1 = findViewById(R.id.panelCutMerge), p2 = findViewById(R.id.panelConvert), p3 = findViewById(R.id.panelMaster), p4 = findViewById(R.id.panelDspLab);
        Button t1 = findViewById(R.id.tabCutMerge), t2 = findViewById(R.id.tabConvert), t3 = findViewById(R.id.tabMaster), t4 = findViewById(R.id.tabDspLab);
        View.OnClickListener sw = v -> {
            p1.setVisibility(v == t1 ? View.VISIBLE : View.GONE); p2.setVisibility(v == t2 ? View.VISIBLE : View.GONE);
            p3.setVisibility(v == t3 ? View.VISIBLE : View.GONE); p4.setVisibility(v == t4 ? View.VISIBLE : View.GONE);
        };
        t1.setOnClickListener(sw); t2.setOnClickListener(sw); t3.setOnClickListener(sw); t4.setOnClickListener(sw);

        findViewById(R.id.btnLoadAudio).setOnClickListener(v -> pickFile(REQ_LOAD));
        findViewById(R.id.btnAppendClip).setOnClickListener(v -> pickFile(REQ_MERGE));
        findViewById(R.id.btnNudgeM10).setOnClickListener(v -> nudge(-10));
        findViewById(R.id.btnNudgeM1).setOnClickListener(v -> nudge(-1));
        findViewById(R.id.btnNudgeP1).setOnClickListener(v -> nudge(1));
        findViewById(R.id.btnNudgeP10).setOnClickListener(v -> nudge(10));

        Button btnJm = findViewById(R.id.btnJogMode);
        btnJm.setOnClickListener(v -> { boolean m = !jogView.isMicroMode(); jogView.setMicroMode(m); btnJm.setText(m ? "MIKRO: 5ms" : "NORMAL: 100ms"); });
        findViewById(R.id.btnPlaySelection).setOnClickListener(v -> { if (durMs > 0) nativeSetPlaybackRange(inMs, outMs, true); });
        findViewById(R.id.btnPlayAll).setOnClickListener(v -> { if (durMs > 0) nativeSetPlaybackRange(0, durMs, true); });
        findViewById(R.id.btnStopAudio).setOnClickListener(v -> nativeTogglePlay(false));

        findViewById(R.id.btnSetIn).setOnClickListener(v -> { if (durMs > 0) { inMs = nativeSnapZeroCrossingMs(posMs); if (inMs >= outMs) outMs = durMs; waveView.setMarkers(inMs, outMs); updateTele(); tvStat.setText("IN SNAP: " + String.format(Locale.US, "%.3fs", inMs/1000.0)); }});
        findViewById(R.id.btnSetOut).setOnClickListener(v -> { if (durMs > 0) { outMs = nativeSnapZeroCrossingMs(posMs); if (outMs <= inMs) inMs = 0; waveView.setMarkers(inMs, outMs); updateTele(); tvStat.setText("OUT SNAP: " + String.format(Locale.US, "%.3fs", outMs/1000.0)); }});
        findViewById(R.id.btnTrimKeep).setOnClickListener(v -> { if (nativeTrimKeepRange(inMs, outMs)) refresh("TRIM SELESAI"); });
        findViewById(R.id.btnRippleDelete).setOnClickListener(v -> { if (nativeRippleDeleteRange(inMs, outMs)) refresh("RIPPLE DELETE SELESAI"); });

        Button btnXf = findViewById(R.id.btnCrossfadeCycle);
        btnXf.setOnClickListener(v -> { int[] o = {0, 100, 250, 500, 1000, 2000}; for (int i=0;i<o.length;i++) if (o[i]==xfadeMs){ xfadeMs=o[(i+1)%o.length]; break; } btnXf.setText("XFADE: " + xfadeMs + "ms"); });
        findViewById(R.id.btnQuickSaveWav).setOnClickListener(v -> exportAudio(2, 48000, 256, 0)); // WAV 32-bit Float Master

        Button bFmt = findViewById(R.id.btnFmtSelect), bRt = findViewById(R.id.btnSampleRateSelect), bBr = findViewById(R.id.btnBitrateSelect), bCh = findViewById(R.id.btnChannelSelect);
        bFmt.setOnClickListener(v -> {
            expFmt = (expFmt + 1) % 5;
            String[] fmtNames = {"FMT: WAV 16-BIT", "FMT: WAV 24-BIT", "FMT: WAV 32f FLOAT", "FMT: FLAC LOSSLESS", "FMT: M4A AAC 320k"};
            bFmt.setText(fmtNames[expFmt]);
        });
        bRt.setOnClickListener(v -> { expRate = expRate == 44100 ? 48000 : (expRate == 48000 ? 96000 : 44100); bRt.setText("RATE: " + expRate + " Hz"); });
        bBr.setOnClickListener(v -> { int[] brOpts = {128, 192, 256, 320}; for (int i=0;i<brOpts.length;i++) if (brOpts[i]==expBr){ expBr=brOpts[(i+1)%brOpts.length]; break; } bBr.setText(expBr + " kbps"); });
        bCh.setOnClickListener(v -> { expCh = (expCh + 1) % 3; String[] chNames = {"CH: STEREO", "CH: MONO", "CH: SWAP L-R"}; bCh.setText(chNames[expCh]); });
        findViewById(R.id.btnExportConverted).setOnClickListener(v -> exportAudio(expFmt, expRate, expBr, expCh));

        findViewById(R.id.btnNorm14).setOnClickListener(v -> asyncRun("Normalizing -14 LUFS...", () -> { float g = nativeNormalizeLufs(-14f); runOnUiThread(() -> refresh(String.format(Locale.US, "NORMALIZED -14 LUFS (%+.1f dB)", g))); }));
        findViewById(R.id.btnNorm16).setOnClickListener(v -> asyncRun("Normalizing -16 LUFS...", () -> { float g = nativeNormalizeLufs(-16f); runOnUiThread(() -> refresh(String.format(Locale.US, "NORMALIZED -16 LUFS (%+.1f dB)", g))); }));
        findViewById(R.id.btnSpectralDenoise).setOnClickListener(v -> asyncRun("FFT Spectral Denoising...", () -> { nativeDenoiseSpectral(0.75f); runOnUiThread(() -> refresh("SPECTRAL DENOISER SELESAI")); }));
        findViewById(R.id.btnEqVocal).setOnClickListener(v -> { nativeApplyMasteringFx(1, 0, 0); refresh("VOCAL CLARITY EQ AKTIF"); });
        findViewById(R.id.btnEqWarmth).setOnClickListener(v -> { nativeApplyMasteringFx(2, 0, 0); refresh("PODCAST WARMTH EQ AKTIF"); });
        findViewById(R.id.btnFadeInOut).setOnClickListener(v -> { nativeApplyMasteringFx(0, 1500, 1500); refresh("FADE-IN & FADE-OUT 1.5s AKTIF"); });

        findViewById(R.id.btnSpeedUp).setOnClickListener(v -> asyncRun("WSOLA Tempo 1.25x...", () -> { nativeTimePitchProcess(1.25f, 0f); runOnUiThread(() -> refresh("TEMPO 1.25x (PITCH TETAP)")); }));
        findViewById(R.id.btnSlowDown).setOnClickListener(v -> asyncRun("WSOLA Tempo 0.80x...", () -> { nativeTimePitchProcess(0.80f, 0f); runOnUiThread(() -> refresh("TEMPO 0.80x (PITCH TETAP)")); }));
        findViewById(R.id.btnPitchUp).setOnClickListener(v -> asyncRun("WSOLA Pitch +2 ST...", () -> { nativeTimePitchProcess(1.0f, 2.0f); runOnUiThread(() -> refresh("PITCH +2 SEMITONE")); }));
        findViewById(R.id.btnVocalRemove).setOnClickListener(v -> { nativeMidSideProcess(1); refresh("KARAOKE (CENTER VOCAL REMOVED)"); });
        findViewById(R.id.btnVocalIsolate).setOnClickListener(v -> { nativeMidSideProcess(2); refresh("ISOLASI VOKAL AKTIF"); });
        findViewById(R.id.btnStripSilence).setOnClickListener(v -> asyncRun("Strip hening otomatis...", () -> { int c = nativeStripSilence(-42f); runOnUiThread(() -> refresh("STRIP HENING: " + c + " JEDA DIPADATKAN")); }));
        findViewById(R.id.btn8DSpatial).setOnClickListener(v -> { nativeApply8DSpatial(); refresh("8D SPATIAL AUDIO AKTIF"); });
        h.post(tick);
    }

    private void nudge(double d) { if (durMs <= 0) return; posMs = Math.max(0, Math.min(durMs, posMs + d)); waveView.setPlayheadMs(posMs, nativeGetZoomSamples(posMs, 25.0, 120)); nativeScrubToMs(posMs, true); updateTele(); }
    private void pickFile(int code) { Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*"); i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"audio/*", "video/*"}); startActivityForResult(i, code); }

    @Override protected void onActivityResult(int req, int res, Intent d) {
        super.onActivityResult(req, res, d);
        if (res != Activity.RESULT_OK || d == null || d.getData() == null) return;
        Uri u = d.getData();
        asyncRun("Mendekode audio/video...", () -> {
            File f = copyUri(u); if (f == null) return;
            boolean ok = (req == REQ_LOAD) ? nativeLoadFile(f.getAbsolutePath()) : nativeAppendMergeFile(f.getAbsolutePath(), xfadeMs);
            runOnUiThread(() -> { if (ok) refresh((req == REQ_LOAD ? "LOADED: " : "MERGED: ") + f.getName()); else tvStat.setText("GAGAL DECODE FILE"); });
        });
    }

    private File copyUri(Uri u) {
        try {
            String nm = "media_in.bin"; Cursor c = getContentResolver().query(u, null, null, null, null);
            if (c != null && c.moveToFirst()) { int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME); if (idx >= 0) nm = c.getString(idx); c.close(); }
            File out = new File(getCacheDir(), nm);
            try (InputStream is = getContentResolver().openInputStream(u); FileOutputStream fos = new FileOutputStream(out)) { byte[] b = new byte[16384]; int n; while ((n = is.read(b)) > 0) fos.write(b, 0, n); }
            return out;
        } catch (Exception e) { return null; }
    }

    private void exportAudio(int fmt, int sr, int br, int ch) {
        if (durMs <= 0) { tvStat.setText("BELUM ADA AUDIO"); return; }
        asyncRun("Merender audio kualitas studio...", () -> {
            File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MUSIC), "AudioForge"); if (!dir.exists()) dir.mkdirs();
            String ext = (fmt == 3) ? ".flac" : ((fmt == 4) ? ".m4a" : ".wav");
            File f = new File(dir, "FORGE_" + new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date()) + ext);
            boolean ok = nativeExportAudio(f.getAbsolutePath(), fmt, sr, br, ch);
            runOnUiThread(() -> { tvStat.setText(ok ? "SAVED: " + f.getAbsolutePath() : "EXPORT GAGAL"); if (ok) Toast.makeText(this, "Tersimpan Lossless: " + f.getName(), Toast.LENGTH_LONG).show(); });
        });
    }

    private void refresh(String m) { durMs = nativeGetDurationMs(); inMs = 0; outMs = durMs; posMs = 0; waveView.updateData(nativeGetWaveformPeaks(160), durMs, inMs, outMs); updateTele(); tvStat.setText(m); }
    private void updateTele() { tvTele.setText(String.format(Locale.US, "IN: %.3fs | OUT: %.3fs | POS: %.3fs | DUR: %.3fs", inMs/1000.0, outMs/1000.0, posMs/1000.0, durMs/1000.0)); }
    private void asyncRun(String m, Runnable r) { tvStat.setText(m); new Thread(r).start(); }
}
