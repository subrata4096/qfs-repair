#!/usr/bin/python

import numpy as np
import matplotlib.pyplot as plt


#N = 50
#x = np.random.rand(N)
#y = np.random.rand(N)
#colors = np.random.rand(N)
#area = np.pi * (15 * np.random.rand(N))**2 # 0 to 15 point radiuses

origFileName = "/home/mitra4/orig_repair_out.txt"
smartFileName = "/home/mitra4/smart_repair_out.txt"

plotName_eps="/home/mitra4/scatter_plot.eps"
plotName_png="/home/mitra4/scatter_plot.png"

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

#repairNum_smart = [2, 2, 1, 1, 2, 3, 2, 3, 5, 5, 5, 4, 8, 9, 8, 7, 13, 10, 7, 7, 4, 9, 14]
#repairTime_smart = [2172214, 2238247, 2116884, 1839824, 1845872, 2245959, 1889686, 2031775, 2239226, 2900724, 3174517, 2456762, 3413051, 4051363, 4138096, 3689755, 3711155, 3854921, 3885098, 3165761, 3533145, 4343022, 3557984]


#repairNum_orig = [2, 3, 2, 3, 2, 2, 3, 2, 4, 4, 4, 4, 8, 8, 9, 9, 10, 10, 10, 10, 12, 12]
#repairTime_orig = [3660827, 4381517, 3669054, 3282411, 3805317, 3143278, 3659845, 3747558, 4244495, 3262264, 3562835, 4008541, 5145834, 4036144, 5621935, 5017549, 5440892, 5953129, 6276125, 5961792, 5458813, 6019428]

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

plt.savefig(plotName_eps, format='eps',dpi=800,bbox_inches='tight')
plt.savefig(plotName_png, format='png',dpi=800,bbox_inches='tight')
plt.show()


