#!/usr/bin/python

import subprocess
import numpy as np

def grepForTimeString(dirName):
        strToSearch = "REPAIR took time"

        fileName = dirName + "/MetaServer.*"
   
        grepCommand = "grep \"" + strToSearch + "\" " + fileName
 
	p = subprocess.Popen(grepCommand, shell=True, stdout=subprocess.PIPE)

        outStr, errStr = p.communicate()   

	#print outStr
   
       
        #return outStr

        retList =[] 

        for line in outStr.split("\n"):
		if(line != "") :
			retList.append(line)
 
	return retList
        #print "Error start  ___________________________________________"
       
        #print errStr

        #print "Error end  ___________________________________________"


def parseStringAndGetTime(theLine, mode):
        theLine = theLine.strip()
	fields = theLine.split(" ")

	
	repairTime = ""

	if(mode == "orig"):
		repairTime = fields[14]
	elif(mode == "repair"):
		repairTime = fields[21]
		
	#print "Time taken = ", repairTime

    	return repairTime


def handleLinesForOneExp(lineList, dirName, mode):
	#print dirName
        timeList = []
        for line in lineList :
        	theTime = parseStringAndGetTime(line, mode)
		timeList.append(int(theTime))

	if(len(timeList) == 0):
		None
	npArr = np.array(timeList)
	
	theMaxTimeTakenForReapir = np.max(npArr)
	theAvgTimeTakenForReapir = np.mean(npArr)

	return (theMaxTimeTakenForReapir, theAvgTimeTakenForReapir)
		

if __name__ == "__main__" :
	
	#chunkSizeList = ["32MB", "64MB", "16MB", "8MB"]
	chunkSizeList = ["64MB"]
  	failureCount = [1, 2 , 4,  8,  16, 32]
        modeList = ["repair", "orig"]
   	
 	#numExpId = ["1", "2", "3", "4", "5"]
 	numExpId = ["1"]

        codes = ["12_4_new"]

        for code in codes:
         for chunkSize in chunkSizeList:
		print " - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - "
		for mode in modeList:
                        print " --------------------------------------------"
			maxTimeList = []
			numOfFailuresRecoveredSuccesfully = []
			for fCount in failureCount:
                                #avgTimeList = []
   				for eId in numExpId:

					dirName = "/home/ubuntu/experimentLogBaseDir/" + code + "/" chunkSize + "/failures_" + str(fCount) + "/" + mode + "/exp_" +  eId

                                        outputStrList = grepForTimeString(dirName)

                                        numberOfMatches = len(outputStrList)

                                        #print dirName ,  " number of matches = ",  numberOfMatches 
					if(numberOfMatches == 0) :
						continue

                                        #if(numberOfMatches != fCount) : 
					#	print "Incomplete run! : found ", numberOfMatches , " instead of ", fCount,  chunkSize + "/failures_" + str(fCount) + "/" + mode + "/exp_" +  eId
						#continue

                                        #print outputStrList
					
					retval = handleLinesForOneExp(outputStrList, dirName, mode)

					if(retval == None):
						continue

                                        mxTime, avgTime = retval

                                        maxTimeList.append(mxTime)
					numOfFailuresRecoveredSuccesfully.append(numberOfMatches)      
                                        #avgTimeList.append(avgTime)      

                                         
				#end for
                                #if((len(maxTimeList) == 0) or (len(avgTimeList) == 0)):
				#	continue
                                #npmaxTimeArr = np.array(maxTimeList)
				#npavgTimeArr = np.array(avgTimeList)

                                #avgOfMax = np.mean(npmaxTimeArr)
                                #avgOfAvg = np.mean(npavgTimeArr)

                                #avgOfMaxInMSec = int(float(avgOfMax/1000.00) + 0.5)
                                #avgOfAvgInMSec = int(float(avgOfAvg/1000.00) + 0.5)

                                #print "For mode:",mode," CHUNKSIZE = ",chunkSize," num failure = ",fCount," avg Maximum REPAIR time = ", avgOfMaxInMSec, " ms and avg Average REPAIR time = ", avgOfAvgInMSec, " ms"
                                #print "For mode:",mode," CHUNKSIZE = ",chunkSize," num failure = ",numberOfMatches," avg Maximum REPAIR time = ", avgOfMaxInMSec, " ms and avg Average REPAIR time = ", avgOfAvgInMSec, " ms"
                        
			#end for
			print chunkSize, mode, " --------------------------------------------"
			print numOfFailuresRecoveredSuccesfully
			print maxTimeList
		#end for
		print " - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - "
                                        

 
