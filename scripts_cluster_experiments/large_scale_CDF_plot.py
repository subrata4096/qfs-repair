#!/usr/bin/python

import numpy as np
import matplotlib.pyplot as plt


#N = 50
#x = np.random.rand(N)
#y = np.random.rand(N)
#colors = np.random.rand(N)
#area = np.pi * (15 * np.random.rand(N))**2 # 0 to 15 point radiuses

cdfFileInputName = "/home/mitra4/orig_repair_out.txt"
cdfFileOutName_png = "/home/mitra4/cdf_256_plot.png"
cdfFileOutName_eps = "/home/mitra4/cdf_256_plot.eps"

repairNum_orig = []
repairTime_orig = []

repairNum_smart = []
repairTime_smart = []

with open(origFileName) as f:
    for line in f:
        parts = line.split(",") # split line into parts
        if len(parts) > 1:   # if at least 2 parts/columns
            repairNum_orig.append(float(parts[0]))   # column 1 is the failure number
            repairTime_orig.append(float(parts[1]))   # column 2 is the rapirtime

with open(smartFileName) as f:
    for line in f:
        parts = line.split(",") # split line into parts
        if len(parts) > 1:   # if at least 2 parts/columns
            repairNum_smart.append(float(parts[0]))   # column 1 is the failure number
            repairTime_smart.append(float(parts[1]))   # column 2 is the rapirtime


print repairNum_orig
print repairTime_orig

colors = ['b', 'c', 'y', 'm', 'r']

org = plt.scatter(repairNum_orig, repairTime_orig, marker='x', color=colors[3], alpha=0.4)
smrt = plt.scatter(repairNum_smart, repairTime_smart, marker='o', color=colors[0], alpha=0.4)

xlabels = [1,2,4,8,16,32,64,128,256]

repairTime_orig = [float(i) for i in repairTime_orig]
repairTime_smart = [float(i) for i in repairTime_smart]

minRepairTimeObserved = np.min(repairTime_smart)
maxRepairTimeObserved = np.max(repairTime_orig)

recalculatedYTicksInMsec = np.linspace(minRepairTimeObserved-1,maxRepairTimeObserved+2,num=10)
recalculatedYTicksInSec = [int((i+0.5)/1000) for i in recalculatedYTicksInMsec]
#recalculatedYTicksInSec = recalculatedYTicksInMsec/1000

plt.xscale('log',basex=2)
plt.xticks(np.logspace(0, 9,num=9,endpoint=True,base=2.0), xlabels)
plt.yticks(recalculatedYTicksInMsec,recalculatedYTicksInSec)

plt.xlabel("Number of simultanious failures", fontsize=20)
plt.ylabel("Repair time of individual repairs (sec)", fontsize=20)

plt.xticks(fontsize=18)
plt.yticks(fontsize=18)

plt.legend((org, smrt),
           ('Traditional repair', 'PPD repair'),
           scatterpoints=1,
           loc='upper left',
           ncol=1,
           fontsize=18)

plt.savefig(cdfFileOutName_eps, format='eps',dpi=800,bbox_inches='tight')
plt.savefig(cdfFileOutName_png, format='png',dpi=800,bbox_inches='tight')
plt.show()


