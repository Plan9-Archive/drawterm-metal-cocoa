package org.echoline.drawterm;

import android.util.Log;

import android.content.Context;
import android.graphics.Bitmap;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.SurfaceHolder;
import android.view.View;
import android.widget.CheckBox;
import android.widget.EditText;

import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import java.security.spec.ECField;

/**
 * Created by eli on 12/3/17.
 */
public class MySurfaceView extends SurfaceView implements SurfaceHolder.Callback {
	private int screenWidth, screenHeight;
	private MainActivity mainActivity;
	private float ws, hs;

	public MySurfaceView(Context context, int w, int h, float ws, float hs) {
		super(context);
		screenHeight = h;
		screenWidth = w;
		this.ws = ws;
		this.hs = hs;
		mainActivity = (MainActivity)context;
		mainActivity.setWidth(screenWidth);
		mainActivity.setHeight(screenHeight);
		mainActivity.setWidthScale(ws);
		mainActivity.setHeightScale(hs);
		setWillNotDraw(true);

		getHolder().addCallback(this);

		setOnTouchListener(new View.OnTouchListener() {
			private int[] mouse = new int[3];

			@Override
			public boolean onTouch(View v, MotionEvent event) {
				CheckBox left = (CheckBox)mainActivity.findViewById(R.id.mouseLeft);
				CheckBox middle = (CheckBox)mainActivity.findViewById(R.id.mouseMiddle);
				CheckBox right = (CheckBox)mainActivity.findViewById(R.id.mouseRight);
				CheckBox up = (CheckBox)mainActivity.findViewById(R.id.mouseUp);
				CheckBox down = (CheckBox)mainActivity.findViewById(R.id.mouseDown);
				int buttons = (left.isChecked()? 1: 0) |
								(middle.isChecked()? 2: 0) |
								(right.isChecked()? 4: 0) |
								(up.isChecked()? 8: 0) |
								(down.isChecked()? 16: 0);
				if (event.getAction() == MotionEvent.ACTION_DOWN) {
					mouse[0] = Math.round(event.getX());
					mouse[1] = Math.round(event.getY());
					mouse[2] = buttons;
					mainActivity.setMouse(mouse);
				} else if (event.getAction() == MotionEvent.ACTION_MOVE) {
					mouse[0] = Math.round(event.getX());
					mouse[1] = Math.round(event.getY());
					mouse[2] = buttons;
					mainActivity.setMouse(mouse);
				} else if (event.getAction() == MotionEvent.ACTION_UP) {
					mouse[0] = Math.round(event.getX());
					mouse[1] = Math.round(event.getY());
					mouse[2] = 0;
					mainActivity.setMouse(mouse);
				}
				return true;
			}
		});
	}

	@Override
	public void surfaceCreated(SurfaceHolder holder) {
		mainActivity.setDTSurface(holder.getSurface());
	}

	@Override
	public void surfaceChanged(SurfaceHolder holder, int w, int h, int format) {
	}

	@Override
	public void surfaceDestroyed(SurfaceHolder holder) {
		mainActivity.setDTSurface(null);
	}
}
