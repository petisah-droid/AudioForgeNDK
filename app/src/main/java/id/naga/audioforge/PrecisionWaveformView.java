package id.naga.audioforge;

import android.content.Context;
import android.graphics.*;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

public class PrecisionWaveformView extends View {
    public interface OnScrubListener { void onScrub(double positionMs, boolean isTouching); }
    private float[] peaks = new float[0], zoomSamples = new float[0];
    private double durationMs = 0.0, playheadMs = 0.0, inMs = 0.0, outMs = 0.0;
    private boolean isTouching = false;
    private float touchX = 0f;
    private OnScrubListener listener;
    private final Paint bgP = new Paint(Paint.ANTI_ALIAS_FLAG), gridP = new Paint(Paint.ANTI_ALIAS_FLAG),
            waveP = new Paint(Paint.ANTI_ALIAS_FLAG), actP = new Paint(Paint.ANTI_ALIAS_FLAG),
            selP = new Paint(Paint.ANTI_ALIAS_FLAG), inP = new Paint(Paint.ANTI_ALIAS_FLAG),
            outP = new Paint(Paint.ANTI_ALIAS_FLAG), playP = new Paint(Paint.ANTI_ALIAS_FLAG),
            lBgP = new Paint(Paint.ANTI_ALIAS_FLAG), lBdP = new Paint(Paint.ANTI_ALIAS_FLAG),
            lWvP = new Paint(Paint.ANTI_ALIAS_FLAG), txtP = new Paint(Paint.ANTI_ALIAS_FLAG);

    public PrecisionWaveformView(Context c, AttributeSet a) {
        super(c, a);
        bgP.setColor(Color.parseColor("#0B0F17")); gridP.setColor(Color.parseColor("#1B2436")); gridP.setStrokeWidth(2f);
        waveP.setColor(Color.parseColor("#334155")); waveP.setStrokeWidth(3f);
        actP.setColor(Color.parseColor("#00E5FF")); actP.setStrokeWidth(3.2f);
        selP.setColor(Color.parseColor("#2200E5FF"));
        inP.setColor(Color.parseColor("#10B981")); inP.setStrokeWidth(4f);
        outP.setColor(Color.parseColor("#F59E0B")); outP.setStrokeWidth(4f);
        playP.setColor(Color.parseColor("#FF3D71")); playP.setStrokeWidth(4f);
        lBgP.setColor(Color.parseColor("#F20F172A")); lBdP.setColor(Color.parseColor("#00E5FF"));
        lBdP.setStyle(Paint.Style.STROKE); lBdP.setStrokeWidth(3.5f);
        lWvP.setColor(Color.parseColor("#38BDF8")); lWvP.setStyle(Paint.Style.STROKE); lWvP.setStrokeWidth(3f);
        txtP.setColor(Color.WHITE); txtP.setTextSize(24f); txtP.setFakeBoldText(true);
    }
    public void setOnScrubListener(OnScrubListener l) { this.listener = l; }
    public void updateData(float[] p, double d, double i, double o) { peaks = p != null ? p : new float[0]; durationMs = d; inMs = i; outMs = o; invalidate(); }
    public void setMarkers(double i, double o) { inMs = i; outMs = o; invalidate(); }
    public void setPlayheadMs(double ms, float[] z) { playheadMs = ms; if (z != null) zoomSamples = z; invalidate(); }

    @Override protected void onDraw(Canvas cv) {
        super.onDraw(cv);
        float w = getWidth(), h = getHeight(), mid = h * 0.5f;
        cv.drawRoundRect(new RectF(0, 0, w, h), 24f, 24f, bgP);
        for (int i = 1; i < 8; i++) cv.drawLine((w/8f)*i, 0, (w/8f)*i, h, gridP);
        cv.drawLine(0, mid, w, mid, gridP);
        if (durationMs <= 0 || peaks.length == 0) {
            txtP.setTextAlign(Paint.Align.CENTER); txtP.setColor(Color.parseColor("#64748B"));
            cv.drawText("TAP '+ LOAD FILE' UNTUK MEMUAT AUDIO / VIDEO", w*0.5f, mid+8f, txtP); return;
        }
        float ix = (float)((inMs/durationMs)*w), ox = (float)((outMs/durationMs)*w), px = (float)((playheadMs/durationMs)*w);
        cv.drawRect(ix, 8f, ox, h-8f, selP);
        float step = w / peaks.length;
        for (int i = 0; i < peaks.length; i++) {
            float x = i*step + step*0.5f, amp = Math.max(0.02f, peaks[i])*(h*0.42f);
            cv.drawLine(x, mid-amp, x, mid+amp, (x >= ix && x <= ox) ? actP : waveP);
        }
        cv.drawLine(ix, 0, ix, h, inP); cv.drawRoundRect(new RectF(ix, 6f, ix+52f, 38f), 8f, 8f, inP);
        txtP.setTextAlign(Paint.Align.LEFT); txtP.setColor(Color.BLACK); txtP.setTextSize(20f);
        cv.drawText("IN", ix+14f, 28f, txtP);
        cv.drawLine(ox, 0, ox, h, outP); cv.drawRoundRect(new RectF(ox-64f, h-38f, ox, h-6f), 8f, 8f, outP);
        cv.drawText("OUT", ox-54f, h-14f, txtP);
        cv.drawLine(px, 0, px, h, playP); cv.drawCircle(px, 14f, 10f, playP);

        if (isTouching && zoomSamples.length > 2) {
            float lw = 310f, lh = 125f, lx = Math.max(14f, Math.min(w-lw-14f, touchX-lw*0.5f)), ly = 12f;
            RectF lr = new RectF(lx, ly, lx+lw, ly+lh);
            cv.drawRoundRect(lr, 18f, 18f, lBgP); cv.drawRoundRect(lr, 18f, 18f, lBdP);
            float lm = ly + lh*0.5f, sx = (lw-24f)/(zoomSamples.length-1);
            for (int i = 1; i < zoomSamples.length; i++)
                cv.drawLine(lx+12f+(i-1)*sx, lm-zoomSamples[i-1]*(lh*0.42f), lx+12f+i*sx, lm-zoomSamples[i]*(lh*0.42f), lWvP);
            cv.drawLine(lx+lw*0.5f, ly+8f, lx+lw*0.5f, ly+lh-8f, playP);
            txtP.setColor(Color.parseColor("#00E5FF")); txtP.setTextSize(19f);
            cv.drawText(String.format("20x LOUPE | %.3f s", playheadMs/1000.0), lx+14f, ly+25f, txtP);
        }
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        if (durationMs <= 0) return false;
        touchX = Math.max(0f, Math.min(getWidth(), e.getX()));
        double ms = (touchX / getWidth()) * durationMs;
        if (e.getAction() == MotionEvent.ACTION_DOWN || e.getAction() == MotionEvent.ACTION_MOVE) {
            isTouching = true; playheadMs = ms; if (listener != null) listener.onScrub(ms, true); invalidate(); return true;
        } else if (e.getAction() == MotionEvent.ACTION_UP || e.getAction() == MotionEvent.ACTION_CANCEL) {
            isTouching = false; if (listener != null) listener.onScrub(ms, false); invalidate(); return true;
        }
        return super.onTouchEvent(e);
    }
}
