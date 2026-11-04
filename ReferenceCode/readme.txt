Preparing the demo:

* each time a file has been loaded the system must be reset afterwards in order to be able to load another file.

1. make sure the board has a file system. If not use "format 0" to create one.
2. load into the file system using the "lf" command and the fileloader utility (set to send file as is):
	letters3d.txt  (give it the name letters3d)
	numbers3d.txt  (give it the name numbers3d)
	motorola3d.txt (give it the name motorola3d)
	marvell3d.txt  (give it the name marvell3d)
3. now load and run (using "ln" and "g") 2 programs: 3dfloor.txt , 3dball.txt. These 2 programs will create 2
   text files in the file system : floor3d, ball3d.
* in order to view the file conetents you can use "type marvell3d"

4. In order to load the BMP file (bubba.bmp) we must use the tftp, but first we must set the environment variables
   that appear on the boot screen.
   There are 2 ways to set the environment:
	1. Load a text file using "lf", called "env.sys" into the file system which contains the following format:

tftpHostIP 10.2.1.137
tftpServerIP 10.2.1.62
tftpDownloadAddress 0x800c0000
eth0MAC 00:00:00:00:00:05
AUTOBOOT file

(each row has to end by pressing return, including the last one). The variable values can be different according
to the system being used (mainly the server IP parameter).

	2. Manually add the variables to the environment:
		envclear
		envadd tftpHostIP 10.2.1.137
		...
		envadd AUTOBOOT file
		envsave
after the environment has been set, reset the board, and the new values will appear on boot.
Now, set the tftp server to the directory where the bmp is and "lftftp bubba.bmp" , after the file is received, 
save it under the name bubbabmp (it may take some time).
5. Load the demo program (demoMain.txt) using "lfexe" and the fileloader, and save it under the name "demo" 
(there will be and .exe extension added to it).

In order to run the demo type "demo" in the command line, or if the AUTOBOOT parameter is set, it will start on reset
after 3 seconds.
*remember to set the PCI voltage to 5V for the video card to work.