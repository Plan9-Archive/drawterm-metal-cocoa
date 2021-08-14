package org.echoline.drawterm;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.Resources;
import android.graphics.Point;
import android.os.Bundle;
import android.os.Environment;

import android.app.Activity;

import android.app.Notification;
import android.app.NotificationManager;

import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.MotionEvent;
import android.view.View;
import android.view.Menu;
import android.view.MenuItem;
import android.view.WindowManager;
import android.view.Surface;
import android.view.inputmethod.InputMethodManager;
import android.view.KeyEvent;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.TotalCaptureResult;
import android.media.Image;
import android.media.ImageReader;
import android.graphics.ImageFormat;
import android.os.Handler;
import android.os.HandlerThread;

import java.io.File;
import java.util.Map;
import java.nio.ByteBuffer;
import java.util.List;
import java.util.ArrayList;

public class MainActivity extends Activity {
	private Map<String, ?> map;
	private MainActivity mainActivity;
	private boolean dtrunning = false;
	private DrawTermThread dthread;
	private int notificationId;
	private CameraDevice cameraDevice = null;
	private byte []jpegBytes;

	static {
		System.loadLibrary("drawterm");
	}

	public void showNotification(String text) {
		Notification.Builder builder = new Notification.Builder(this)
			.setDefaults(Notification.DEFAULT_SOUND)
			.setSmallIcon(R.drawable.ic_small)
			.setContentText(text)
			.setStyle(new Notification.BigTextStyle().bigText(text))
			.setPriority(Notification.PRIORITY_DEFAULT);

		((NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE)).notify(notificationId, builder.build());
		notificationId++;
	}

	public int numCameras() {
		try {
			return ((CameraManager)getSystemService(Context.CAMERA_SERVICE)).getCameraIdList().length;
		} catch (CameraAccessException e) {
			Log.w("drawterm", e.toString());
			return 0;
		}
	}

	public void takePicture(int id) {
		try {
			HandlerThread mBackgroundThread = new HandlerThread("Camera Background");
			mBackgroundThread.start();
			Handler mBackgroundHandler = new Handler(mBackgroundThread.getLooper());
			CameraManager manager = (CameraManager)getSystemService(Context.CAMERA_SERVICE);
			String []cameraIdList = manager.getCameraIdList();
			manager.openCamera(cameraIdList[id], new CameraDevice.StateCallback() {
				public void onOpened(CameraDevice device) {
					cameraDevice = device;
				}
				public void onDisconnected(CameraDevice device) {
					if (cameraDevice != null)
						cameraDevice.close();
					cameraDevice = null;
				}
				public void onError(CameraDevice device, int error) {
					if (cameraDevice != null)
						cameraDevice.close();
					cameraDevice = null;
				}
			}, mBackgroundHandler);
			ImageReader reader = ImageReader.newInstance(640, 480, ImageFormat.JPEG, 1);
			CaptureRequest.Builder captureBuilder = cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_ZERO_SHUTTER_LAG);
			captureBuilder.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO);
			captureBuilder.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_AUTO);
			captureBuilder.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON);
			captureBuilder.set(CaptureRequest.JPEG_ORIENTATION, getWindowManager().getDefaultDisplay().getRotation());
			captureBuilder.addTarget(reader.getSurface());
			reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
				public void onImageAvailable(ImageReader reader) {
					Image image = null;
					try {
						image = reader.acquireLatestImage();
						ByteBuffer buffer = image.getPlanes()[0].getBuffer();
						jpegBytes = new byte[buffer.capacity()];
						buffer.get(jpegBytes);
					} catch (Exception e) {
						Log.w("drawterm", e.toString());
					} finally {
						if (image != null) {
							image.close();
						}
					}
				}
			}, mBackgroundHandler);
			List<Surface> outputSurfaces = new ArrayList<Surface>(1);
			outputSurfaces.add(reader.getSurface());
			cameraDevice.createCaptureSession(outputSurfaces, new CameraCaptureSession.StateCallback() {
				public void onConfigured(CameraCaptureSession session) {
					try {
						List<CaptureRequest> captureRequests = new ArrayList<CaptureRequest>(10);
						for (int i = 0; i < 10; i++)
							captureRequests.add(captureBuilder.build());
						session.captureBurst(captureRequests, new CameraCaptureSession.CaptureCallback() {
							public void onCaptureSequenceCompleted(CameraCaptureSession session, int sequenceId, long frameNumber) {
								try {
									sendPicture(jpegBytes);
									mBackgroundThread.quitSafely();
									mBackgroundThread.join();
								} catch (Exception e) {
									Log.w("drawterm", e.toString());
								}
							}
						}, mBackgroundHandler);
					} catch (CameraAccessException e) {
						e.printStackTrace();
					}
				}
				public void onConfigureFailed(CameraCaptureSession session) {
				}
			}, mBackgroundHandler);
		} catch (Exception e) {
			e.printStackTrace();
		}
	}

	public void serverView(View v) {
		setContentView(R.layout.server_main);
		serverButtons();

		String s = (String)map.get(((TextView)v).getText().toString());
		String []a = s.split("\007");

		((EditText)findViewById(R.id.cpuServer)).setText((String)a[0]);
		((EditText)findViewById(R.id.authServer)).setText((String)a[1]);
		((EditText)findViewById(R.id.userName)).setText((String)a[2]);
		if (a.length > 3)
			((EditText)findViewById(R.id.passWord)).setText((String)a[3]);
	}

	public void populateServers(Context context) {
		ListView ll = (ListView)findViewById(R.id.servers);
		ArrayAdapter<String> la = new ArrayAdapter<String>(this, R.layout.item_main);
		SharedPreferences settings = getSharedPreferences("DrawtermPrefs", 0);
		map = (Map<String, ?>)settings.getAll();
		String key;
		Object []keys = map.keySet().toArray();
		for (int i = 0; i < keys.length; i++) {
			key = (String)keys[i];
			la.add(key);
		}
		ll.setAdapter(la);

		setDTSurface(null);
		dtrunning = false;
	}

	public void runDrawterm(String []args, String pass) {
		Resources res = getResources();
		DisplayMetrics dm = res.getDisplayMetrics();

		int wp = dm.widthPixels;
		int hp = dm.heightPixels;

		setContentView(R.layout.drawterm_main);

		Button kbutton = (Button)findViewById(R.id.keyboardToggle);
		kbutton.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(final View view) {
				InputMethodManager imm = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
				imm.toggleSoftInput(InputMethodManager.SHOW_IMPLICIT, 0);
			}
		});

		int rid = res.getIdentifier("navigation_bar_height", "dimen", "android");
		if (rid > 0) {
			hp -= res.getDimensionPixelSize(rid);
		}
		LinearLayout ll = (LinearLayout)findViewById(R.id.dtButtons);
		hp -= ll.getHeight();

		int w = (int)(wp * (160.0/dm.xdpi));
		int h = (int)(hp * (160.0/dm.ydpi));
		float ws = (float)wp/w;
		float hs = (float)hp/h;
		// only scale up
		if (ws < 1) {
			ws = 1;
			w = wp;
		}
		if (hs < 1) {
			hs = 1;
			h = hp;
		}

		MySurfaceView mView = new MySurfaceView(mainActivity, w, h, ws, hs);
		mView.getHolder().setFixedSize(w, h);

		LinearLayout l = (LinearLayout)findViewById(R.id.dlayout);
		l.addView(mView, 1, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT));

		dthread = new DrawTermThread(args, pass, mainActivity);
		dthread.start();

		dtrunning = true;
	}

	public void serverButtons() {
		Button button = (Button)findViewById(R.id.save);
		button.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(View v) {
				String cpu = ((EditText)findViewById(R.id.cpuServer)).getText().toString();
				String auth = ((EditText)findViewById(R.id.authServer)).getText().toString();
				String user = ((EditText)findViewById(R.id.userName)).getText().toString();
				String pass = ((EditText)findViewById(R.id.passWord)).getText().toString();

				SharedPreferences settings = getSharedPreferences("DrawtermPrefs", 0);
				SharedPreferences.Editor editor = settings.edit();
				editor.putString(user + "@" + cpu + " (auth="  + auth + ")", cpu + "\007" + auth + "\007" + user + "\007" + pass);
				editor.commit();
			}
		});

		button = (Button) findViewById(R.id.connect);
		button.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(final View view) {
				String cpu = ((EditText)findViewById(R.id.cpuServer)).getText().toString();
				String auth = ((EditText)findViewById(R.id.authServer)).getText().toString();
				String user = ((EditText)findViewById(R.id.userName)).getText().toString();
				String pass = ((EditText)findViewById(R.id.passWord)).getText().toString();

				String args[] = {"drawterm", "-p", "-h", cpu, "-a", auth, "-u", user};
				runDrawterm(args, pass);
			}
		});
	}

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

		mainActivity = this;
		setObject();
		setContentView(R.layout.activity_main);
		populateServers(this);

		View fab = findViewById(R.id.fab);
		fab.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(View v) {
				setContentView(R.layout.server_main);
				serverButtons();
			}
		});
	}

	@Override
	public boolean dispatchKeyEvent(KeyEvent event)
	{
		if (!dtrunning) {
			return super.dispatchKeyEvent(event);
		}

		int k = event.getUnicodeChar();
		if (k == 0) {
			k = event.getDisplayLabel();
			if (k >= 'A' && k <= 'Z')
				k |= 0x20;
		}
		String chars = event.getCharacters();
		if (k == 0 && chars != null) {
			for (int i = 0; i < chars.length(); i++) {
				k = chars.codePointAt(i);
				keyDown(k);
				keyUp(k);
			}
			return true;
		}

		if (k == 0) switch (event.getKeyCode()) {
		case KeyEvent.KEYCODE_DEL:
			k = 0x0008;
			break;
		case KeyEvent.KEYCODE_FORWARD_DEL:
			k = 0x007F;
			break;
		case KeyEvent.KEYCODE_ESCAPE:
			k = 0x001B;
			break;
		case KeyEvent.KEYCODE_MOVE_HOME:
			k = 0xF00D;
			break;
		case KeyEvent.KEYCODE_MOVE_END:
			k = 0xF018;
			break;
		case KeyEvent.KEYCODE_PAGE_UP:
			k = 0xF00F;
			break;
		case KeyEvent.KEYCODE_PAGE_DOWN:
			k = 0xF013;
			break;
		case KeyEvent.KEYCODE_INSERT:
			k = 0xF014;
			break;
		case KeyEvent.KEYCODE_SYSRQ:
			k = 0xF010;
			break;
		case KeyEvent.KEYCODE_DPAD_UP:
			k = 0xF00E;
			break;
		case KeyEvent.KEYCODE_DPAD_LEFT:
			k = 0xF011;
			break;
		case KeyEvent.KEYCODE_DPAD_RIGHT:
			k = 0xF012;
			break;
		case KeyEvent.KEYCODE_DPAD_DOWN:
			k = 0xF800;
			break;
		}

		if (k == 0)
			return true;

		if (event.isCtrlPressed()) {
			keyDown(0xF017);
		}
		if (event.isAltPressed() && k < 128) {
			keyDown(0xF015);
		}

		if (event.getAction() == KeyEvent.ACTION_DOWN) {
			keyDown(k);
		}
		else if (event.getAction() == KeyEvent.ACTION_UP) {
			keyUp(k);
		}

		if (event.isCtrlPressed()) {
			keyUp(0xF017);
		}
		if (event.isAltPressed() && k < 128) {
			keyUp(0xF015);
		}

		return true;
	}

	@Override
	public void onBackPressed()
	{
	}

	@Override
	public void onDestroy()
	{
		setDTSurface(null);
		dtrunning = false;
		exitDT();
		super.onDestroy();
	}

	public void setClipBoard(String str) {
		ClipboardManager cm = (ClipboardManager)getApplicationContext().getSystemService(Context.CLIPBOARD_SERVICE);
		if (cm != null) {
			ClipData cd = ClipData.newPlainText(null, str);
			cm.setPrimaryClip(cd);
		}
	}

	public String getClipBoard() {
		ClipboardManager cm = (ClipboardManager)getApplicationContext().getSystemService(Context.CLIPBOARD_SERVICE);
		if (cm != null) {
			ClipData cd = cm.getPrimaryClip();
			if (cd != null)
				return (String)(cd.getItemAt(0).coerceToText(mainActivity.getApplicationContext()).toString());
		}
		return "";
	}

	public native void dtmain(Object[] args);
	public native void setPass(String arg);
	public native void setWidth(int arg);
	public native void setHeight(int arg);
	public native void setWidthScale(float arg);
	public native void setHeightScale(float arg);
	public native void setDTSurface(Surface surface);
	public native void setMouse(int[] args);
	public native void setObject();
	public native void keyDown(int c);
	public native void keyUp(int c);
	public native void exitDT();
	public native void sendPicture(byte[] array);
}
