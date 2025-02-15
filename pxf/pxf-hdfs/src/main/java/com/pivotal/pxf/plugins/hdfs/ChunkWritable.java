package com.pivotal.pxf.plugins.hdfs;

import java.io.DataOutput;
import java.io.DataInput;
import java.io.IOException;
import java.lang.UnsupportedOperationException;

import org.apache.hadoop.io.Writable;

/**
 * Just an output buffer for the ChunkRecordReader. It must extend Writable
 * otherwise it will not fit into the next() interface method
 */
public class ChunkWritable implements Writable {
	public byte [] box;
	
	/**
     * Serialize the fields of this object to <code>out</code>.
     *
     * @param out <code>DataOutput</code> to serialize this object into.
     * @throws IOException
     */
	@Override
    public void write(DataOutput out)  {
		throw new UnsupportedOperationException("ChunkWritable.write() is not implemented");
    }
	
    /**
     * Deserialize the fields of this object from <code>in</code>.
     * <p>For efficiency, implementations should attempt to re-use storage in the
     * existing object where possible.</p>
     *
     * @param in <code>DataInput</code> to deserialize this object from.
     * @throws IOException
     */
	@Override
    public void readFields(DataInput in)  {
		throw new UnsupportedOperationException("ChunkWritable.readFields() is not implemented");
	}
	
}