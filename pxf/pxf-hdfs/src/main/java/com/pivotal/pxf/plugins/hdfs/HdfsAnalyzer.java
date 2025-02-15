package com.pivotal.pxf.plugins.hdfs;

import com.pivotal.pxf.api.Analyzer;
import com.pivotal.pxf.api.AnalyzerStats;
import com.pivotal.pxf.api.ReadAccessor;
import com.pivotal.pxf.api.utilities.InputData;
import com.pivotal.pxf.service.ReadBridge;
import com.pivotal.pxf.plugins.hdfs.utilities.HdfsUtilities;
import com.pivotal.pxf.plugins.hdfs.utilities.PxfInputFormat;
import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.FileStatus;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.mapred.FileSplit;
import org.apache.hadoop.mapred.InputSplit;
import org.apache.hadoop.mapred.JobConf;

import java.io.IOException;
import java.util.ArrayList;


/**
 * Analyzer class for HDFS data resources
 *
 * Given an HDFS data source (a file, directory, or wild card pattern)
 * return statistics about it (number of blocks, number of tuples, etc.)
 */
public class HdfsAnalyzer extends Analyzer {
    private JobConf jobConf;
    private FileSystem fs;
    private Log Log;

    /**
     * Constructs an HdfsAnalyzer object
     * 
     * @param inputData all input parameters coming from the client
     * @throws IOException
     */
    public HdfsAnalyzer(InputData inputData) throws IOException {
        super(inputData);
        Log = LogFactory.getLog(HdfsAnalyzer.class);

        jobConf = new JobConf(new Configuration(), HdfsAnalyzer.class);
        fs = FileSystem.get(jobConf);
    }

    /**
     * Collects a number of basic statistics based on an estimate. Statistics
     * are: number of records, number of hdfs blocks and hdfs block size.
     * 
     * @param datapath path is a data source URI that can appear as a file 
     *        name, a directory name or a wildcard pattern
     * @return statistics in json format
     * @throws Exception
     */
    @Override
    public AnalyzerStats getEstimatedStats(String datapath) throws Exception {
        long blockSize = 0;
        long numberOfBlocks;
        Path path = new Path(HdfsUtilities.absoluteDataPath(datapath));

        ArrayList<InputSplit> splits = getSplits(path);

        for (InputSplit split : splits) {
            FileSplit fsp = (FileSplit) split;
            Path filePath = fsp.getPath();
            FileStatus fileStatus = fs.getFileStatus(filePath);
            if (fileStatus.isFile()) {
                blockSize = fileStatus.getBlockSize();
                break;
            }
        }

        // if no file is in path (only dirs), get default block size
        if (blockSize == 0) {
            blockSize = fs.getDefaultBlockSize(path);
        }
        numberOfBlocks = splits.size();


        long numberOfTuplesInBlock = getNumberOfTuplesInBlock(splits);
        AnalyzerStats stats = new AnalyzerStats(blockSize, numberOfBlocks, numberOfTuplesInBlock * numberOfBlocks);

        //print files size to log when in debug level
        Log.debug(AnalyzerStats.dataToString(stats, path.toString()));

        return stats;
    }

    /*
     * Calculate the number of tuples in a split (block)
     * Reads one block from HDFS. Exception during reading will
     * filter upwards and handled in AnalyzerResource
     */
    private long getNumberOfTuplesInBlock(ArrayList<InputSplit> splits) throws Exception {
        long tuples = -1; /* default  - if we are not able to read data */
        ReadAccessor accessor;

        if (splits.isEmpty()) {
            return 0;
        }

        /*
         * metadata information includes: file split's
         * start, length and hosts (locations).
         */
        FileSplit firstSplit = (FileSplit) splits.get(0);
        byte[] fragmentMetadata = HdfsUtilities.prepareFragmentMetadata(firstSplit);
        inputData.setFragmentMetadata(fragmentMetadata);
        inputData.setDataSource(firstSplit.getPath().toUri().getPath());
        accessor = ReadBridge.getFileAccessor(inputData);

        if (accessor.openForRead()) {
            tuples = 0;
            while (accessor.readNextObject() != null) {
                tuples++;
            }

            accessor.closeForRead();
        }

        return tuples;
    }

    private ArrayList<InputSplit> getSplits(Path path) throws IOException {
        PxfInputFormat fformat = new PxfInputFormat();
        PxfInputFormat.setInputPaths(jobConf, path);
        InputSplit[] splits = fformat.getSplits(jobConf, 1);
        ArrayList<InputSplit> result = new ArrayList<InputSplit>();
        
        // remove empty splits
        if (splits != null) {
	        for (InputSplit split : splits) {
	        	if (split.getLength() > 0) {
	        		result.add(split);
	        	}
	        }
        }
        
        return result;        
    }
}
