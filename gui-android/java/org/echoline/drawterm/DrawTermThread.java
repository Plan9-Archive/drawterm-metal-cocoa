package org.echoline.drawterm;

/**
 * Created by eli on 12/4/17.
 */

public class DrawTermThread extends Thread {
	private MainActivity m;
	private String p;
	private String []args;

	public DrawTermThread(String []args, String p, MainActivity m) {
		this.m = m;
		this.p = p;
		this.args = args;
	}

	@Override
	public void run() {
		if (p != null && !p.equals(""))
			m.setPass(p);
		m.dtmain(args);
		m.runOnUiThread(new Runnable() {
			@Override
			public void run() {
				m.exitDT();
				m.setContentView(R.layout.server_main);
				m.populateServers(m);
			}
		});
	}
}
