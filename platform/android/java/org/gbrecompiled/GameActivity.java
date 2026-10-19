package org.gbrecompiled;

import android.os.Bundle;
import android.system.Os;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import java.util.HashSet;
import org.libsdl.app.SDLActivity;

public final class GameActivity extends SDLActivity {
    private final HashSet<Integer> held=new HashSet<>();
    @Override protected void onCreate(Bundle saved) {
        try {
            String base=getFilesDir().getAbsolutePath();
            Os.setenv("GBRECOMP_STATE_DIR",base,true);
            Os.setenv("GBRECOMP_ASSET_DIR",base,true);
            Os.setenv("GBRECOMP_ROM_PATH",base+"/game.rom",true);
        } catch(Exception failure) { throw new IllegalStateException(failure); }
        super.onCreate(saved);
        FrameLayout overlay=new FrameLayout(this);
        FrameLayout pad=new FrameLayout(this);
        pad.setVisibility(View.GONE);
        overlay.addView(pad,new FrameLayout.LayoutParams(-1,-1));
        Button toggle=new Button(this);toggle.setText("Touch controls");toggle.setAlpha(.65f);
        FrameLayout.LayoutParams t=new FrameLayout.LayoutParams(-2,dp(44),Gravity.TOP|Gravity.RIGHT);
        overlay.addView(toggle,t);
        toggle.setOnClickListener(v -> { release();pad.setVisibility(pad.getVisibility()==View.VISIBLE?View.GONE:View.VISIBLE); });
        key(pad,"↑",KeyEvent.KEYCODE_DPAD_UP,Gravity.BOTTOM|Gravity.LEFT,60,110);
        key(pad,"↓",KeyEvent.KEYCODE_DPAD_DOWN,Gravity.BOTTOM|Gravity.LEFT,60,0);
        key(pad,"←",KeyEvent.KEYCODE_DPAD_LEFT,Gravity.BOTTOM|Gravity.LEFT,0,55);
        key(pad,"→",KeyEvent.KEYCODE_DPAD_RIGHT,Gravity.BOTTOM|Gravity.LEFT,120,55);
        key(pad,"A",KeyEvent.KEYCODE_Z,Gravity.BOTTOM|Gravity.RIGHT,0,80);
        key(pad,"B",KeyEvent.KEYCODE_X,Gravity.BOTTOM|Gravity.RIGHT,65,20);
        key(pad,"Start",KeyEvent.KEYCODE_ENTER,Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL,55,0);
        key(pad,"Select",KeyEvent.KEYCODE_DEL,Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL,-55,0);
        key(pad,"Rewind",KeyEvent.KEYCODE_R,Gravity.TOP|Gravity.LEFT,0,0);
        addContentView(overlay,new FrameLayout.LayoutParams(-1,-1));
    }
    private int dp(int value) { return (int)(value*getResources().getDisplayMetrics().density+.5f); }
    private void key(FrameLayout pad,String text,int code,int gravity,int side,int bottom) {
        Button button=new Button(this);button.setText(text);button.setAlpha(.5f);button.setFocusable(false);
        FrameLayout.LayoutParams pos=new FrameLayout.LayoutParams(dp(64),dp(52),gravity);
        if((gravity&Gravity.HORIZONTAL_GRAVITY_MASK)==Gravity.RIGHT)pos.rightMargin=dp(side);
        else pos.leftMargin=dp(side);
        pos.bottomMargin=dp(bottom);pad.addView(button,pos);
        button.setOnTouchListener((v,event) -> {
            if(event.getActionMasked()==MotionEvent.ACTION_DOWN) { if(held.add(code))onNativeKeyDown(code);v.setPressed(true); }
            if(event.getActionMasked()==MotionEvent.ACTION_UP || event.getActionMasked()==MotionEvent.ACTION_CANCEL) { if(held.remove(code))onNativeKeyUp(code);v.setPressed(false); }
            return true;
        });
    }
    private void release() { for(int code:held)onNativeKeyUp(code);held.clear(); }
    @Override protected void onPause() { release();super.onPause(); }
}
