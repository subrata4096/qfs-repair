#!/usr/bin/python

import numpy as np
import matplotlib.pyplot as plt


#N = 50
#x = np.random.rand(N)
#y = np.random.rand(N)
#colors = np.random.rand(N)
#area = np.pi * (15 * np.random.rand(N))**2 # 0 to 15 point radiuses


repairNum_smart = [2, 2, 1, 1, 2, 3, 2, 3, 5, 5, 5, 4, 8, 9, 8, 7, 13, 10, 7, 7, 4, 9, 14]
repairTime_smart = [2172214, 2238247, 2116884, 1839824, 1845872, 2245959, 1889686, 2031775, 2239226, 2900724, 3174517, 2456762, 3413051, 4051363, 4138096, 3689755, 3711155, 3854921, 3885098, 3165761, 3533145, 4343022, 3557984]


repairNum_orig = [2, 3, 2, 3, 2, 2, 3, 2, 4, 4, 4, 4, 8, 8, 9, 9, 10, 10, 10, 10, 12, 12]
repairTime_orig = [3660827, 4381517, 3669054, 3282411, 3805317, 3143278, 3659845, 3747558, 4244495, 3262264, 3562835, 4008541, 5145834, 4036144, 5621935, 5017549, 5440892, 5953129, 6276125, 5961792, 5458813, 6019428]

plt.scatter(repairNum_orig, repairTime_orig, c=r, alpha=0.5)
plt.scatter(repairNum_smart, repairTime_smart, c=b, alpha=0.5)

plt.show()


