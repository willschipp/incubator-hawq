package com.pivotal.pxf.plugins.hive.utilities;

import java.util.ArrayList;

import org.apache.commons.lang.StringUtils;
import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;
import org.apache.hadoop.hive.conf.HiveConf;
import org.apache.hadoop.hive.metastore.HiveMetaStoreClient;
import org.apache.hadoop.hive.metastore.TableType;
import org.apache.hadoop.hive.metastore.api.FieldSchema;
import org.apache.hadoop.hive.metastore.api.MetaException;
import org.apache.hadoop.hive.metastore.api.Table;

import com.pivotal.pxf.api.Metadata;
import com.pivotal.pxf.api.UnsupportedTypeException;

/**
 * Class containing helper functions connecting
 * and interacting with Hive.
 */
public class HiveUtilities {

    private static final Log LOG = LogFactory.getLog(HiveUtilities.class);

    /**
     * Default Hive DB (schema) name.
     */
    private static final String HIVE_DEFAULT_DBNAME = "default";

    /**
     * Initializes the HiveMetaStoreClient
     * Uses classpath configuration files to locate the MetaStore
     *
     * @return initialized client
     */
    public static HiveMetaStoreClient initHiveClient() {
        HiveMetaStoreClient client = null;
        try {
            client = new HiveMetaStoreClient(new HiveConf());
        } catch (MetaException cause) {
            throw new RuntimeException("Failed connecting to Hive MetaStore service: " + cause.getMessage(), cause);
        }
        return client;
    }

    public static Table getHiveTable(HiveMetaStoreClient client, Metadata.Table tableName)
            throws Exception {
        Table tbl = client.getTable(tableName.getDbName(), tableName.getTableName());
        String tblType = tbl.getTableType();

        if (LOG.isDebugEnabled()) {
            LOG.debug("Table: " + tableName.getDbName() + "." + tableName.getTableName() + ", type: " + tblType);
        }

        if (TableType.valueOf(tblType) == TableType.VIRTUAL_VIEW) {
            throw new UnsupportedOperationException("Hive views are not supported by HAWQ");
        }

        return tbl;
    }

    /**
     * Checks if hive type is supported, and if so
     * return its matching HAWQ type.
     * Unsupported types will result in an exception.
     *
     * The supported mappings are:
     * tinyint -> int2
     * smallint -> int2
     * int -> int4
     * bigint -> int8
     * boolean -> bool
     * float -> float4
     * double -> float8
     * string -> text
     * binary -> bytea
     * timestamp -> timestamp
     * date -> date
     * decimal(precision, scale) -> numeric(precision, scale)
     * varchar(size) -> varchar(size)
     * char(size) -> bpchar(size)
     *
     * @param hiveColumn hive column schema
     * @return field with mapped HAWQ type and modifiers
     */
    public static Metadata.Field mapHiveType(FieldSchema hiveColumn) throws UnsupportedTypeException {
        String fieldName = hiveColumn.getName();
        String hiveType = hiveColumn.getType();
        String mappedType;
        String[] modifiers = null;

        // check parameterized types:
        if (hiveType.startsWith("varchar(") ||
                hiveType.startsWith("char(")) {
            String[] toks = hiveType.split("[(,)]");
            if (toks.length != 2) {
                throw new UnsupportedTypeException( "HAWQ does not support type " + hiveType + " (Field " + fieldName + "), " +
                        "expected type of the form <type name>(<parameter>)");
            }
            mappedType = toks[0];
            if (mappedType.equals("char")) {
                mappedType = "bpchar";
            }
            modifiers = new String[] {toks[1]};
        } else if (hiveType.startsWith("decimal(")) {
            String[] toks = hiveType.split("[(,)]");
            if (toks.length != 3) {
                throw new UnsupportedTypeException( "HAWQ does not support type " + hiveType + " (Field " + fieldName + "), " +
                        "expected type of the form <type name>(<parameter>,<parameter>)");
            }
            mappedType = "numeric";
            modifiers = new String[] {toks[1], toks[2]};
        } else {

            switch (hiveType) {
            case "tinyint":
            case "smallint":
            	mappedType = "int2";
            	break;
            case "int":
            	mappedType = "int4";
            	break;
            case "bigint":
            	mappedType = "int8";
            	break;
            case "boolean":
            	mappedType = "bool";
            	break;
            case "timestamp":
            case "date":
                mappedType = hiveType;
                break;
            case "float":
                mappedType = "float4";
                break;
            case "double":
                mappedType = "float8";
                break;
            case "string":
                mappedType = "text";
                break;
            case "binary":
                mappedType = "bytea";
                break;
            default:
                throw new UnsupportedTypeException(
                        "HAWQ does not support type " + hiveType + " (Field " + fieldName + ")");
            }
        }
        if (!verifyModifers(modifiers)) {
            throw new UnsupportedTypeException("HAWQ does not support type " + hiveType + " (Field " + fieldName + "), modifiers should be integers");
        }
        return new Metadata.Field(fieldName, mappedType, modifiers);
    }

    /**
     * Verifies modifiers are null or integers.
     *
     * @param modifiers type modifiers to be verified
     */
    private static boolean verifyModifers(String[] modifiers) {
        if (modifiers == null) {
            return true;
        }
        for (String modifier: modifiers) {
            if (StringUtils.isBlank(modifier) || !StringUtils.isNumeric(modifier)) {
                return false;
            }
        }
        return true;
    }

    /**
     * Extracts the db_name and table_name from the qualifiedName.
     * qualifiedName is the Hive table name that the user enters in the CREATE EXTERNAL TABLE statement
     * or when querying HCatalog table.
     * It can be either <table_name> or <db_name.table_name>.
     */
    public static Metadata.Table parseTableQualifiedName(String qualifiedName) {

        String dbName, tableName;
        String errorMsg = " is not a valid Hive table name. "
                + "Should be either <table_name> or <db_name.table_name>";

        if (StringUtils.isBlank(qualifiedName)) {
            throw new IllegalArgumentException("empty string" + errorMsg);
        }

        String[] rawToks = qualifiedName.split("[.]");
        ArrayList<String> toks = new ArrayList<String>();
        for (String tok: rawToks) {
            if (StringUtils.isBlank(tok)) {
                continue;
            }
            toks.add(tok.trim());
        }

        if (toks.size() == 1) {
            dbName = HIVE_DEFAULT_DBNAME;
            tableName = toks.get(0);
        } else if (toks.size() == 2) {
            dbName = toks.get(0);
            tableName = toks.get(1);
        } else {
            throw new IllegalArgumentException("\"" + qualifiedName + "\"" + errorMsg);
        }

        return new Metadata.Table(dbName, tableName);
    }
}
