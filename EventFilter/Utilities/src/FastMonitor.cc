/*
 * FastMonitor.cc
 *
 *  Created on: Nov 27, 2012
 *      Author: aspataru
 */

#include "EventFilter/Utilities/interface/FastMonitor.h"
#include "EventFilter/Utilities/interface/JsonSerializable.h"
#include "EventFilter/Utilities/interface/FileIO.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>

using namespace jsoncollector;

FastMonitor::FastMonitor(std::string const& defPath, bool strictChecking, bool useSource, bool useDefinition) :
	defPath_(defPath),strictChecking_(strictChecking),useSource_(useSource),useDefinition_(useDefinition),nStreams_(1)
{
	//get host and PID info
	if (useSource)
	  getHostAndPID(sourceInfo_);

	//load definition file
	DataPointDefinition::getDataPointDefinitionFor(defPath_, dpd_);

}

FastMonitor::~FastMonitor() {
  for (auto dp: dataPoints_) delete dp;
}

void FastMonitor::addFastPathDefinition(std::string const& defPathFast, bool strict) {
   haveFastPath_=true;
   defpathFast_=defPathFast;
   DataPointDefinition::getDataPointDefinitionFor(defPathFast_, dpdFast_);
   fastPathStrictChecking_=strict;
}

//per-process variables
void FastMonitor::registerGlobalMonitorable(JsonMonitorable *newMonitorable, bool NAifZeroUpdates, unsigned int *nBins)
{
	DataPoint *dp = new DataPoint(sourceInfo_,defPath_);
	dp->trackMonitorable(newMonitorable,NAifZeroUpdates);
	dp->setNBins(nBins);
	dataPoints_.push_back(dp);
	dpNameMap_[newMonitorable->getName()]=dataPoints_.size()-1;

	//check if same name is registered twice
	assert(uids_.insert(newMonitorable->getName()).second);
}

//fast path: no merge operation is performed
void FastMonitor::registerFastGlobalMonitorable(JsonMonitorable *newMonitorable)
{
        DataPoint *dp = new DataPoint(sourceInfo_,defPathFast_,true);
        dp->trackMonitorable(newMonitorable,false);
        dataPointsFast_.push_back(dp);
}

//per-stream variables
void FastMonitor::registerStreamMonitorableUIntVec(std::string const& name, 
		std::vector<unsigned int> *inputs, bool NAifZeroUpdates, unsigned int *nBins)
{
	DataPoint *dp = new DataPoint(sourceInfo_,defPath_);
	dp->trackVectorUInt(name,inputs,NAifZeroUpdates);
	dp->setNBins(nBins);
        dataPoints_.push_back(dp);
	dpNameMap_[name]=dataPoints_.size()-1;

	assert (uids_.insert(name).second);
}


//atomic variables with guaranteed updates at the time of reading
void FastMonitor::registerStreamMonitorableUIntVecAtomic(std::string const& name, 
		std::vector<AtomicMonUInt*> *inputs, bool NAifZeroUpdates, unsigned int *nBins)
{
	std::string definitionToPass;
	if (useDefinition_) definitionToPass=defPath_;
	DataPoint *dp = new DataPoint(definitionToPass,sourceInfo_);
	dp->trackVectorUIntAtomic(name,inputs,NAifZeroUpdates);
	dp->setNBins(nBins);
        dataPoints_.push_back(dp);
	dpNameMap_[name]=dataPoints_.size()-1;

	assert (uids_.insert(name).second);
}



void FastMonitor::commit(std::vector<unsigned int> *streamLumisPtr)
{
  std::vector<std::string> const& jsonNames= dpd_.getNames();
  regDpCount_ = dataPoints_.size();
  assert(!(strictChecking_ && jsonNames.size()==regDpCount_));

  std::map<unsigned int,bool> hasJson;
  for (unsigned int i=0;i<jsonNames.size();i++)
  {
    bool notFoundVar=true;
    for (unsigned int j=0;j<regDpCount_;j++) {
      if (dataPoints_[j]->getName()==jsonNames[i])
      {
	dataPoints_[j]->setOperation(dpd_.getOperationFor(i));
	jsonDpIndex_.push_back(j);
	hasJson[j]=true;
	notFoundVar=false;
	break;
      }
    }
    if (notFoundVar) {
      assert(!strictChecking_);
      //push dummy DP if not registered by the service so that we output required JSON/CSV
      DataPoint *dummyDp = new DataPoint(sourceInfo_,defPath_);
      dummyDp->trackDummy(jsonNames[i],true);
      dataPoints_.push_back(dummyDp);
      jsonDpIndex_.push_back(dataPoints_.size()-1);
    }
  }
  for (unsigned int i=0;i<regDpCount_;i++) {
    dataPoints_[i]->setStreamLumiPtr(streamLumisPtr);
  }

  //fast path:
  if (haveFastPath_) {
    std::vector<std::string> const& fjsonNames = dpdFast_.getNames();
    fregDpCount_ = dataPointsFast_.size();
    assert(!(fastStrictChecking_ && fjsonNames.size()==fregDpCount_));
    std::map<unsigned int,bool> fhasJson;
    for (unsigned int i=0;i<fjsonNames.size();i++)
    {
      bool notFoundVar=true;
      for (unsigned int j=0;j<fregDpCount_;j++) {
	if (dataPointsFast_[j]->getName()==fjsonNames[i])
	{
	  jsonDpIndexFast_.push_back(j);
	  fhasJson[j]=true;
	  notFoundVar=false;
	  break;
	}
      }
      if (notFoundVar_)
      {
	assert(!strictChecking_);
	//push dummy DP if not registered by the service so that we output required JSON/CSV
	DataPoint *dummyDp = new DataPoint(sourceInfo_,defPathFast_);
	dummyDp->trackDummy(fjsonNames[i],true);
	dataPointsFast_.push_back(dummyDp);
	jsonDpIndexFast_.push_back(dataPoints_.size()-1);

      }
    }
  } 
}

//update everything
void FastMonitor::snap(bool outputCSVFile, std::string const& path, unsigned int forLumi) {
  recentSnaps_++;
  recentSnapsTimer_++;
  for (unsigned int i=0;i<regDpCount_;i++) {
    dataPoints_[i]->snap(forLumi);
  }
  if (outputCSVFile) outputCSV(path);
}

//update for global variables as most of them are correct only at global EOL
void FastMonitor::snapGlobal(bool outputCSVFile, std::string const& path, unsigned int forLumi) {

  recentSnaps_++;
  for (unsigned int i=0;i<regDpCount_;i++) {
    dataPoints_[i]->snapGlobal(forLumi);
  }
  if (outputCSVFile) outputCSV(path);
}

//update atomic per-stream vars(e.g. event counters) not updating time-based measurements (mini/microstate)
void FastMonitor::snapStreamAtomic(bool outputCSVFile, std::string const& path, unsigned int streamID, unsigned int forLumi)
{
  recentSnaps_++;
  for (unsigned int i=0;i<regDpCount_;i++) {
    dataPoints_[i]->snapStreamAtomic(streamID, forLumi);
  }
  if (outputCSVFile) outputCSV(path);

}

void FastMonitor::outputCSV(std::string const& path)
{
    //output what was specified in JSON in the same order (including dummies)
    unsigned int monSize = jsonDpIndex_.size();
    std::stringstream ss;
    if (monSize)
      for (unsigned int j=0; j< monSize;j++) { 
	ss << dataPoints_[jsonDpIndex_[j]]->fastOutCSV();
	if (j<monSize-1) ss << ",";
      }
    std::ofstream outputFile;
    outputFile.open(path.c_str(), std::fstream::out | std::fstream::trunc);
    outputFile << defPath_ << std::endl;
    outputFile << ss.str();
    outputFile << std::endl;
    outputFile.close();
}

//get one variable (caller must delete it later)
JsonMonitorable* FastMonitor::getMergedIntJForLumi(std::string const& name,unsigned int forLumi)
{
  auto it = dpNameMap_.find(name);
  assert(it!=dpNameMap_.end());
  return  dataPoints_[it->second]->mergeAndRetrieveValue(forLumi);
}

bool FastMonitor::outputFullJSON(std::string const& path, unsigned int lumi) {

	std::cout << "SNAP updates: " <<  recentSnaps_ << " (by timer: " << recentSnapsTimer_ 
		<< ") in lumisection " << lumi << std::endl;

        recentSnaps_ = recentSnapsTimer_ = 0;

        Json::Value serializeRoot;
        for (unsigned int j=0; j< jsonDpIndex_.size();j++) { 
          dataPoints_[jsonDpIndex_[j]]->mergeAndSerialize(serializeRoot,lumi,j==0);
	}

        Json::StyledWriter writer;
	std::string && result = writer.write(serializeRoot);
        FileIO::writeStringToFile(path, result);
	return true;
}

void FastMonitor::discardCollected(unsigned int forLumi) {
	for (auto dp: dataPoints_) dp->discardCollected(forLumi);
}

void FastMonitor::getHostAndPID(std::string& sHPid)
{
	std::stringstream hpid;
	int pid = (int) getpid();
	char hostname[128];
	gethostname(hostname, sizeof hostname);
	hpid << hostname << "_" << pid;
	sHPid = hpid.str();
}

