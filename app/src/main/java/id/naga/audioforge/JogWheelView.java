package id.naga.audioforge;

import android.content.Context;
import android.graphics.*;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

public class JogWheelView extends View {
    public interface OnJogDeltaListener { void onJogDeltaMs(double deltaMs); }
    private float lastX = 0f, offset = 0f;
    private boolean micro = false;
    private OnJogDeltaListener listener;
    private final Paint bgP = new Paint(Paint.ANTI_ALIAS_FLAG), bdP = new Paint(Paint.ANTI_ALIAS_FLAG),
            nP = new Paint(Paint.ANTI_ALIAS_FLAG), cP = new Paint(Paint.ANTI_ALIAS_FLAG), tP = new Paint(Paint.ANTI_ALIAS_FLAG);

    public JogWheelView(Context c, AttributeSet a) {
        super(c, a);
        bgP.setColor(Color.parseColor("#111827")); bdP.setColor(Color.parseColor("#1E293B"));
        bdP.setStyle(Paint.Style.STROKE); bdP.setStrokeWidth(3f);
        nP.setColor(Color.parseColor("#38BDF8")); nP.setStrokeWidth(4f);
        cP.setColor(Color.parseColor("#F59E0B")); cP.setStrokeWidth(5f);
        tP.setColor(Color.parseColor("#94A3B8")); tP.setTextSize(21f); tP.setTextAlign(Paint.Align.CENTER); tP.setFakeBoldText(true);
    }
    public void setOnJogDeltaListener(OnJogDeltaListener l) { listener = l; }
    public void setMicroMode(boolean m) { micro = m; nP.setColor(Color.parseColor(m ? "#F59E0B" : "#38BDF8")); invalidate(); }
    public boolean isMicroMode() { return micro; }

    @Override protected void onDraw(Canvas cv) {
        super.onDraw(cv);
        float w = getWidth(), h = getHeight();
        RectF r = new RectF(2f, 2f, w-2f, h-2f);
        cv.drawRoundRect(r, 20f, 20f, bgP); cv.drawRoundRect(r, 20f, 20f, bdP);
        for (float x = (offset % 28f); x < w; x += 28f) {
            nP.setAlpha((int)(255 * Math.max(0.15f, 1f - Math.abs(x - w*0.5f)/(w*0.5f))));
            cv.drawLine(x, h*0.25f, x, h*0.75f, nP);
        }
        cv.drawLine(w*0.5f, 8f, w*0.5f, h-8f, cP);
        cv.drawText(micro ? "◀ GESER JOG-DIAL (MIKRO: 5ms) ▶" : "◀ GESER JOG-DIAL (NORMAL: 100ms) ▶", w*0.5f, h*0.5f+7f, tP);
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        if (e.getAction() == MotionEvent.ACTION_DOWN) { lastX = e.getX(); return true; }
        if (e.getAction() == MotionEvent.ACTION_MOVE) {
            float dx = e.getX() - lastX; lastX = e.getX(); offset += dx;
            if (listener != null) listener.onJogDeltaMs(dx * (micro ? 0.15 : 3.5));
            invalidate(); return true;
        }
        return super.onTouchEvent(e);
    }
}
